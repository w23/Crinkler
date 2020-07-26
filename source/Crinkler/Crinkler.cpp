#include "Crinkler.h"
#include "../Compressor/Compressor.h"
#include "Fix.h"

#include <set>
#include <ctime>
#include <cstring>
#include <climits>

#ifdef WIN32
#include <ppl.h>
#endif

#include "HunkList.h"
#include "Hunk.h"
#include "CoffObjectLoader.h"
#include "CoffLibraryLoader.h"
#include "ImportHandler.h"
#include "Log.h"
#include "HeuristicHunkSorter.h"
#include "ExplicitHunkSorter.h"
#include "EmpiricalHunkSorter.h"
#include "misc.h"
#include "data.h"
#include "Symbol.h"
#include "HtmlReport.h"
#include "NameMangling.h"
#include "MemoryFile.h"

#ifndef WIN32
#define IMAGE_SUBSYSTEM_WINDOWS_GUI 2
#define IMAGE_SUBSYSTEM_WINDOWS_CUI 1
static inline int fopen_s(FILE **out, const char* filename, const char *mode) {
	*out = fopen(filename, mode);
	return *out == nullptr;
}
#endif

using namespace std;

static int PreviousPrime(int n) {
in:
	n = (n - 2) | 1;
	for (int i = 3; i * i < n; i += 2) {
		if (n / i * i == n) goto in;
	}
	return n;
}

static void VerboseLabels(CompressionReportRecord* csr) {
	if(csr->type & RECORD_ROOT) {
		printf("\nlabel name                                   pos comp-pos      size compsize");
	} else {
		string strippedName = StripCrinklerSymbolPrefix(csr->name.c_str());
		if(csr->type & RECORD_SECTION)
			printf("\n%-38.38s", strippedName.c_str());
		else if(csr->type & RECORD_OLD_SECTION)
			printf("  %-36.36s", strippedName.c_str());
		else if(csr->type & RECORD_PUBLIC)
			printf("    %-34.34s", strippedName.c_str());
		else
			printf("      %-32.32s", strippedName.c_str());

		if(csr->compressedPos >= 0)
			printf(" %9d %8.2f %9d %8.2f\n", csr->pos, csr->compressedPos / (BIT_PRECISION *8.0f), csr->size, csr->compressedSize / (BIT_PRECISION *8.0f));
		else
			printf(" %9d          %9d\n", csr->pos, csr->size);
	}

	for(CompressionReportRecord* record : csr->children)
		VerboseLabels(record);
}

static void ProgressUpdateCallback(void* userData, int n, int max)
{
	ProgressBar* progressBar = (ProgressBar*)userData;
	progressBar->Update(n, max);
}

static void NotCrinklerFileError() {
	Log::Error("", "Input file is not a Crinkler compressed executable");
}

Crinkler::Crinkler():
	m_subsystem(SUBSYSTEM_WINDOWS),
	m_hashsize(100*1024*1024),
	m_compressionType(COMPRESSION_FAST),
	m_reuseType(REUSE_OFF),
	m_useSafeImporting(true),
	m_hashtries(0),
	m_hunktries(0),
	m_printFlags(0),
	m_showProgressBar(false),
	m_useTinyHeader(false),
	m_useTinyImport(false),
	m_summaryFilename(""),
	m_truncateFloats(false),
	m_truncateBits(64),
	m_overrideAlignments(false),
	m_unalignCode(false),
	m_alignmentBits(0),
	m_runInitializers(1),
	m_largeAddressAware(0),
	m_saturate(0),
	m_stripExports(false)
{
	InitCompressor();

	m_modellist1 = InstantModels4k();
	m_modellist2 = InstantModels4k();
}


Crinkler::~Crinkler() {
}

void Crinkler::ReplaceDlls(HunkList& hunklist) {
	set<string> usedDlls;
	// Replace DLL
	for(int i = 0; i < hunklist.GetNumHunks(); i++) {
		Hunk* hunk = hunklist[i];
		if(hunk->GetFlags() & HUNK_IS_IMPORT) {
			map<string, string>::iterator it = m_replaceDlls.find(ToLower(hunk->GetImportDll()));
			if(it != m_replaceDlls.end()) {
				hunk->SetImportDll(it->second.c_str());
				usedDlls.insert(it->first);
			}
		}
	}

	// Warn about unused replace DLLs
	for(const auto& p : m_replaceDlls) {
		if(usedDlls.find(p.first) == usedDlls.end()) {
			Log::Warning("", "No functions were imported from replaced dll '%s'", p.first.c_str());
		}
	}
}


void Crinkler::OverrideAlignments(HunkList& hunklist) {
	for(int i = 0; i < hunklist.GetNumHunks(); i++) {
		Hunk* hunk = hunklist[i];
		hunk->OverrideAlignment(m_alignmentBits);
	}
}

void Crinkler::Load(const char* filename) {
	HunkList* hunkList = m_hunkLoader.LoadFromFile(filename);
	if(hunkList) {
		m_hunkPool.Append(hunkList);
		delete hunkList;
	} else {
		Log::Error(filename, "Unsupported file type");
	}
}

void Crinkler::Load(const char* data, int size, const char* module) {
	HunkList* hunkList = m_hunkLoader.Load(data, size, module);
	m_hunkPool.Append(hunkList);
	delete hunkList;
}

void Crinkler::AddRuntimeLibrary() {
	// Add minimal console entry point
	HunkList* runtime = m_hunkLoader.Load(runtimeObj, int(runtimeObj_end - runtimeObj), "runtime");
	m_hunkPool.Append(runtime);
	delete runtime;

	// Add imports from msvcrt
	HunkList* hunklist = new HunkList;
	ForEachExportInDLL("msvcrt", [&](const char* name) {
		string symbolName = name[0] == '?' ? name : string("_") + name;
		string importName = string("__imp_" + symbolName);
		hunklist->AddHunkBack(new Hunk(importName.c_str(), name, "msvcrt"));
		hunklist->AddHunkBack(MakeCallStub(symbolName.c_str()));
	});
	hunklist->MarkHunksAsLibrary();
	m_hunkPool.Append(hunklist);
	delete hunklist;
}

std::string Crinkler::GetEntrySymbolName() const {
	if(m_entry.empty()) {
		switch(m_subsystem) {
			case SUBSYSTEM_CONSOLE:
				return "mainCRTStartup";
			case SUBSYSTEM_WINDOWS:
				return "WinMainCRTStartup";
		}
		return "";
	}
	return m_entry;
}

Symbol*	Crinkler::FindEntryPoint() {
	// Place entry point in the beginning
	string entryName = GetEntrySymbolName();
	Symbol* entry = m_hunkPool.FindUndecoratedSymbol(entryName.c_str());
	if(entry == NULL) {
		Log::Error("", "Cannot find entry point '%s'. See manual for details.", entryName.c_str());
		return NULL;
	}

	if(entry->value > 0) {
		Log::Warning("", "Entry point not at start of section, jump necessary");
	}

	return entry;
}

void Crinkler::RemoveUnreferencedHunks(Hunk* base)
{
	// Check dependencies and remove unused hunks
	vector<Hunk*> startHunks;
	startHunks.push_back(base);

	// Keep hold of exported symbols
	for (const Export& e : m_exports)  {
		if (e.HasValue()) {
			Symbol* sym = m_hunkPool.FindSymbol(e.GetName().c_str());
			if (sym && !sym->fromLibrary) {
				Log::Error("", "Cannot create integer symbol '%s' for export: symbol already exists.", e.GetName().c_str());
			}
		} else {
			Symbol* sym = m_hunkPool.FindSymbol(e.GetSymbol().c_str());
			if (sym) {
				if (sym->hunk->GetRawSize() == 0) {
					sym->hunk->SetRawSize(sym->hunk->GetVirtualSize());
					Log::Warning("", "Uninitialized hunk '%s' forced to data section because of exported symbol '%s'.", sym->hunk->GetName(), e.GetSymbol().c_str());
				}
				startHunks.push_back(sym->hunk);
			} else {
				Log::Error("", "Cannot find symbol '%s' to be exported under name '%s'.", e.GetSymbol().c_str(), e.GetName().c_str());
			}
		}
	}

	// Hack to ensure that LoadLibrary & MessageBox is there to be used in the import code
	Symbol* loadLibrary = m_hunkPool.FindSymbol("__imp__LoadLibraryA@4"); 
	Symbol* messageBox = m_hunkPool.FindSymbol("__imp__MessageBoxA@16");
	Symbol* dynamicInitializers = m_hunkPool.FindSymbol("__DynamicInitializers");
	if(loadLibrary != NULL)
		startHunks.push_back(loadLibrary->hunk);
	if(m_useSafeImporting && !m_useTinyImport && messageBox != NULL)
		startHunks.push_back(messageBox->hunk);
	if(dynamicInitializers != NULL)
		startHunks.push_back(dynamicInitializers->hunk);

	m_hunkPool.RemoveUnreferencedHunks(startHunks);
}

void Crinkler::LoadImportCode(bool use1kMode, bool useSafeImporting, bool useDllFallback, bool useRangeImport) {
	// Do imports
	if (use1kMode){
		Load(import1KObj, int(import1KObj_end - import1KObj), "Crinkler import");
	} else {
		if (useSafeImporting)
			if (useDllFallback)
				if (useRangeImport)
					Load(importSafeFallbackRangeObj, int(importSafeFallbackRangeObj_end - importSafeFallbackRangeObj), "Crinkler import");
				else
					Load(importSafeFallbackObj, int(importSafeFallbackObj_end - importSafeFallbackObj), "Crinkler import");
			else
				if (useRangeImport)
					Load(importSafeRangeObj, int(importSafeRangeObj_end - importSafeRangeObj), "Crinkler import");
				else
					Load(importSafeObj, int(importSafeObj_end - importSafeObj), "Crinkler import");
		else
			if (useDllFallback)
				Log::Error("", "DLL fallback cannot be used with unsafe importing");
			else
				if (useRangeImport)
					Load(importRangeObj, int(importRangeObj_end - importRangeObj), "Crinkler import");
				else
					Load(importObj, int(importObj_end - importObj), "Crinkler import");
	}
}

Hunk* Crinkler::CreateModelHunk(int splittingPoint, int rawsize) {
	Hunk* models;
	int modelsSize = 16 + m_modellist1.nmodels + m_modellist2.nmodels;
	unsigned char masks1[256];
	unsigned char masks2[256];
	unsigned int w1 = m_modellist1.GetMaskList(masks1, false);
	unsigned int w2 = m_modellist2.GetMaskList(masks2, true);
	models = new Hunk("models", 0, 0, 0, modelsSize, modelsSize);
	models->AddSymbol(new Symbol("_Models", 0, SYMBOL_IS_RELOCATEABLE, models));
	char* ptr = models->GetPtr();
	*(unsigned int*)ptr = -(CRINKLER_CODEBASE+splittingPoint);		ptr += sizeof(unsigned int);
	*(unsigned int*)ptr = w1;										ptr += sizeof(unsigned int);
	for(int m = 0; m < m_modellist1.nmodels; m++)
		*ptr++ = masks1[m];
	*(unsigned int*)ptr = -(CRINKLER_CODEBASE+rawsize);				ptr += sizeof(unsigned int);
	*(unsigned int*)ptr = w2;										ptr += sizeof(unsigned int);
	for(int m = 0; m < m_modellist2.nmodels; m++)
		*ptr++ = masks2[m];
	return models;
}

int Crinkler::OptimizeHashsize(unsigned char* data, int datasize, int hashsize, int splittingPoint, int tries) {
	if(tries == 0)
		return hashsize;

	int maxsize = datasize*2+1000;
	int bestsize = INT_MAX;
	int best_hashsize = hashsize;
	m_progressBar.BeginTask("Optimizing hash table size");

	unsigned char context[MAX_CONTEXT_LENGTH] = {};
	HashBits hashbits[2];
	hashbits[0] = ComputeHashBits(data, splittingPoint, context, m_modellist1, true, false);
	hashbits[1] = ComputeHashBits(data + splittingPoint, datasize - splittingPoint, context, m_modellist2, false, true);

	uint32_t* hashsizes = new uint32_t[tries];
	for (int i = 0; i < tries; i++) {
		hashsize = PreviousPrime(hashsize / 2) * 2;
		hashsizes[i] = hashsize;
	}

	int* sizes = new int[tries];

	int progress = 0;
	concurrency::combinable<vector<unsigned char>> buffers([maxsize]() { return vector<unsigned char>(maxsize, 0); });
	concurrency::combinable<vector<TinyHashEntry>> hashtable1([&hashbits]() { return vector<TinyHashEntry>(hashbits[0].tinyhashsize); });
	concurrency::combinable<vector<TinyHashEntry>> hashtable2([&hashbits]() { return vector<TinyHashEntry>(hashbits[1].tinyhashsize); });
	concurrency::critical_section cs;
	concurrency::parallel_for(0, tries, [&](int i) {
		TinyHashEntry* hashtables[] = { hashtable1.local().data(), hashtable2.local().data() };
		sizes[i] = CompressFromHashBits4k(hashbits, hashtables, 2, buffers.local().data(), maxsize, m_saturate != 0, CRINKLER_BASEPROB, hashsizes[i], nullptr);

		Concurrency::critical_section::scoped_lock l(cs);
		m_progressBar.Update(++progress, m_hashtries);
	});

	for (int i = 0; i < tries; i++) {
		if (sizes[i] <= bestsize) {
			bestsize = sizes[i];
			best_hashsize = hashsizes[i];
		}
	}
	delete[] sizes;
	delete[] hashsizes;

	m_progressBar.EndTask();
	
	return best_hashsize;
}

int Crinkler::EstimateModels(unsigned char* data, int datasize, int splittingPoint, bool reestimate, bool use1kMode, int target_size1, int target_size2)
{
	bool verbose = (m_printFlags & PRINT_MODELS) != 0;

	if (use1kMode)
	{
		m_progressBar.BeginTask(reestimate ? "Reestimating models" : "Estimating models");
		int size = target_size1;
		int new_size;
		ModelList1k new_modellist1k = ApproximateModels1k(data, datasize, &new_size, ProgressUpdateCallback, &m_progressBar);
		if(new_size < size)
		{
			size = new_size;
			m_modellist1k = new_modellist1k;
		}
		m_progressBar.EndTask();
		printf("\nEstimated compressed size: %.2f\n", size / (float)(BIT_PRECISION * 8));
		if(verbose) m_modellist1k.Print();
		return new_size;
	}
	else
	{
		unsigned char contexts[2][MAX_CONTEXT_LENGTH] = {};
		for (int i = 0; i < MAX_CONTEXT_LENGTH; i++)
		{
			int srcpos = splittingPoint - MAX_CONTEXT_LENGTH + i;
			contexts[1][i] = srcpos >= 0 ? data[srcpos] : 0;
		}

		int size1 = target_size1;
		int size2 = target_size2;
		ModelList4k modellist1, modellist2;

		int new_size1, new_size2;
		m_progressBar.BeginTask(reestimate ? "Reestimating models for code" : "Estimating models for code");
		modellist1 = ApproximateModels4k(data, splittingPoint, contexts[0], m_compressionType, m_saturate != 0, CRINKLER_BASEPROB, &new_size1, ProgressUpdateCallback, &m_progressBar);
		m_progressBar.EndTask();

		if(new_size1 < size1)
		{
			size1 = new_size1;
			m_modellist1 = modellist1;
		}
		if (verbose) {
			printf("Models: ");
			m_modellist1.Print(stdout);
		}
		printf("Estimated compressed size of code: %.2f\n", size1 / (float)(BIT_PRECISION * 8));

		m_progressBar.BeginTask(reestimate ? "Reestimating models for data" : "Estimating models for data");
		modellist2 = ApproximateModels4k(data + splittingPoint, datasize - splittingPoint, contexts[1], m_compressionType, m_saturate != 0, CRINKLER_BASEPROB, &new_size2, ProgressUpdateCallback, &m_progressBar);
		m_progressBar.EndTask();

		if(new_size2 < size2)
		{
			size2 = new_size2;
			m_modellist2 = modellist2;
		}
		if (verbose) {
			printf("Models: ");
			m_modellist2.Print(stdout);
		}
		printf("Estimated compressed size of data: %.2f\n", size2 / (float)(BIT_PRECISION * 8));

		ModelList4k* modelLists[] = {&m_modellist1, &m_modellist2};
		int segmentSizes[] = { splittingPoint, datasize - splittingPoint };
		int compressedSizes[2] = {};
		int idealsize = EvaluateSize4k(data, 2, segmentSizes, compressedSizes, modelLists, CRINKLER_BASEPROB, m_saturate != 0);
		printf("\nIdeal compressed size of code: %.2f\n", compressedSizes[0] / (float)(BIT_PRECISION * 8));
		printf("Ideal compressed size of data: %.2f\n", compressedSizes[1] / (float)(BIT_PRECISION * 8));
		printf("Ideal compressed total size: %.2f\n", idealsize / (float)(BIT_PRECISION * 8));

		return idealsize;
	}
}

void Crinkler::SetHeaderSaturation(Hunk* header) {
	if (m_saturate) {
		static const unsigned char saturateCode[] = { 0x75, 0x03, 0xFE, 0x0C, 0x1F };
		header->Insert(header->FindSymbol("_SaturatePtr")->value, saturateCode, sizeof(saturateCode));
		*(header->GetPtr() + header->FindSymbol("_SaturateAdjust1Ptr")->value) += sizeof(saturateCode);
		*(header->GetPtr() + header->FindSymbol("_SaturateAdjust2Ptr")->value) -= sizeof(saturateCode);
	}
}

void Crinkler::SetHeaderConstants(Hunk* header, Hunk* phase1, int hashsize, int boostfactor, int baseprob0, int baseprob1, unsigned int modelmask, int subsystem_version, int exports_rva, bool use1kHeader)
{
	header->AddSymbol(new Symbol("_HashTableSize", hashsize/2, 0, header));
	header->AddSymbol(new Symbol("_UnpackedData", CRINKLER_CODEBASE, 0, header));
	header->AddSymbol(new Symbol("_ImageBase", CRINKLER_IMAGEBASE, 0, header));
	header->AddSymbol(new Symbol("_ModelMask", modelmask, 0, header));

	if (use1kHeader)
	{
		int virtualSizeHighByteOffset = header->FindSymbol("_VirtualSizeHighBytePtr")->value;
		int lowBytes = *(int*)(header->GetPtr() + virtualSizeHighByteOffset - 3) & 0xFFFFFF;
		int virtualSize = phase1->GetVirtualSize() + 65536 * 2;
		
		*(header->GetPtr() + header->FindSymbol("_BaseProbPtr0")->value) = baseprob0;
		*(header->GetPtr() + header->FindSymbol("_BaseProbPtr1")->value) = baseprob1;
		*(header->GetPtr() + header->FindSymbol("_BoostFactorPtr")->value) = boostfactor;
		*(unsigned short*)(header->GetPtr() + header->FindSymbol("_DepackEndPositionPtr")->value) = phase1->GetRawSize() + CRINKLER_CODEBASE;
		*(header->GetPtr() + virtualSizeHighByteOffset) = (virtualSize - lowBytes + 0xFFFFFF) >> 24;
	}
	else
	{
		int virtualSize = Align(max(phase1->GetVirtualSize(), phase1->GetRawSize() + hashsize), 16);
		header->AddSymbol(new Symbol("_VirtualSize", virtualSize, 0, header));
		*(header->GetPtr() + header->FindSymbol("_BaseProbPtr")->value) = CRINKLER_BASEPROB;
		*(header->GetPtr() + header->FindSymbol("_ModelSkipPtr")->value) = m_modellist1.nmodels + 8;
		if (exports_rva) {
			*(int*)(header->GetPtr() + header->FindSymbol("_ExportTableRVAPtr")->value) = exports_rva;
			*(int*)(header->GetPtr() + header->FindSymbol("_NumberOfDataDirectoriesPtr")->value) = 1;
		}
	}
	
	*(header->GetPtr() + header->FindSymbol("_SubsystemTypePtr")->value) = subsystem_version;
	*((short*)(header->GetPtr() + header->FindSymbol("_LinkerVersionPtr")->value)) = CRINKLER_LINKER_VERSION;

	if (phase1->GetRawSize() >= 2 && (phase1->GetPtr()[0] == 0x5F || phase1->GetPtr()[2] == 0x5F))
	{
		// Code starts with POP EDI => call transform
		*(header->GetPtr() + header->FindSymbol("_SpareNopPtr")->value) = 0x57; // PUSH EDI
	}
	if (m_largeAddressAware)
	{
		*((short*)(header->GetPtr() + header->FindSymbol("_CharacteristicsPtr")->value)) |= 0x0020;
	}
}

void Crinkler::Recompress(const char* input_filename, const char* output_filename) {
#ifndef WIN32
	// not supported
#else
	MemoryFile file(input_filename);
	unsigned char* indata = (unsigned char*)file.GetPtr();

	FILE* outfile = 0;
	if (strcmp(input_filename, output_filename) != 0) {
		// Open output file now, just to be sure
		if(fopen_s(&outfile, output_filename, "wb")) {
			Log::Error("", "Cannot open '%s' for writing", output_filename);
			return;
		}
	}

	int length = file.GetSize();
	if(length < 200)
	{
		NotCrinklerFileError();
	}

	unsigned int pe_header_offset = *(unsigned int*)&indata[0x3C];

	bool is_compatibility_header = false;
	
	bool is_tiny_header = false;
	char majorlv = 0, minorlv = 0;

	if(pe_header_offset == 4)
	{
		is_compatibility_header = false;
		majorlv = indata[2];
		minorlv = indata[3];
		if(majorlv >= '2' && indata[0xC] == 0x0F && indata[0xD] == 0xA3 && indata[0xE] == 0x2D)
		{
			is_tiny_header = true;
		}
	}
	else if(pe_header_offset == 12)
	{
		is_compatibility_header = true;
		majorlv = indata[38];
		minorlv = indata[39];
	}
	else
	{
		NotCrinklerFileError();
	}
	
	if (majorlv < '0' || majorlv > '9' ||
		minorlv < '0' || minorlv > '9') {
			NotCrinklerFileError();
	}

	// Oops: 0.6 -> 1.0
	if (majorlv == '0' && minorlv == '6') {
		majorlv = '1';
		minorlv = '0';
	}
	int version = (majorlv-'0')*10 + (minorlv-'0');

	if (is_compatibility_header && version >= 14) {
		printf("File compressed using a pre-1.4 Crinkler and recompressed using Crinkler version %c.%c\n", majorlv, minorlv);
	} else {
		printf("File compressed or recompressed using Crinkler version %c.%c\n", majorlv, minorlv);
	}

	switch(majorlv) {
		case '0':
			switch(minorlv) {
				case '1':
				case '2':
				case '3':
					Log::Error("", "Only files compressed using Crinkler 0.4 or newer can be recompressed.\n");
					return;
					break;
				case '4':
				case '5':
					FixHeader04((char*)indata);
					break;
			}
			break;
		case '1':
			switch(minorlv) {
				case '0':
					FixHeader10((char*)indata);
					break;
			}
			break;
	}


	int virtualSize = (*(int*)&indata[pe_header_offset+0x50]) - 0x20000;
	int hashtable_size = -1;
	int return_offset = -1;
	int models_address = -1;
	int depacker_start = -1;
	int rawsize_start = -1;
	int compressed_data_rva = -1;
	for(int i = 0; i < 0x200; i++)
	{
		if(is_tiny_header)
		{
			if(indata[i] == 0x7C && indata[i + 2] == 0xC3 && return_offset == -1) {
				return_offset = i + 2;
				indata[return_offset] = 0xCC;
			}

			if(indata[i] == 0x66 && indata[i + 1] == 0x81 && indata[i + 2] == 0xff)
			{
				rawsize_start = i + 3;
			}

			if(version <= 21)
			{
				if(indata[i] == 0xB9 && indata[i + 1] == 0x00 && indata[i + 2] == 0x00 && indata[i + 3] == 0x00 && indata[i + 4] == 0x00 &&
					indata[i + 5] == 0x59 && indata[i + 6] == 0x6a)
				{
					m_modellist1k.baseprob0 = indata[i + 7];
					m_modellist1k.baseprob1 = indata[i + 9];
					m_modellist1k.modelmask = *(unsigned int*)&indata[i + 11];
				}
			}
			else
			{
				if(indata[i] == 0x6a && indata[i + 2] == 0x3d && indata[i + 3] == 0x00 && indata[i + 4] == 0x00 && indata[i + 5] == 0x00 &&
					indata[i + 6] == 0x00 && indata[i + 7] == 0x6a )
				{
					m_modellist1k.baseprob0 = indata[i + 1];
					m_modellist1k.baseprob1 = indata[i + 8];
					m_modellist1k.modelmask = *(unsigned int*)&indata[i + 10];
				}
			}

			if(indata[i] == 0x7F && indata[i + 2] == 0xB1 && indata[i + 4] == 0x89 && indata[i + 5] == 0xE6)
			{
				m_modellist1k.boost = indata[i + 3];
			}

			if(indata[i] == 0x0F && indata[i + 1] == 0xA3 && indata[i + 2] == 0x2D && compressed_data_rva == -1)
			{
				compressed_data_rva = *(int*)&indata[i + 3];
			}
		}
		else
		{
			if(indata[i] == 0xbf && indata[i + 5] == 0xb9 && hashtable_size == -1) {
				hashtable_size = (*(int*)&indata[i + 6]) * 2;
			}
			if(indata[i] == 0x5A && indata[i + 1] == 0x7B && indata[i + 3] == 0xC3 && return_offset == -1) {
				return_offset = i + 3;
				indata[return_offset] = 0xCC;
			}
			else if(indata[i] == 0x8D && indata[i + 3] == 0x7B && indata[i + 5] == 0xC3 && return_offset == -1) {
				return_offset = i + 5;
				indata[return_offset] = 0xCC;
			}

			if(version < 13)
			{
				if(indata[i] == 0x4B && indata[i + 1] == 0x61 && indata[i + 2] == 0x7F) {
					depacker_start = i;
				}
			}
			else if(version == 13)
			{
				if(indata[i] == 0x0F && indata[i + 1] == 0xA3 && indata[i + 2] == 0x2D) {
					depacker_start = i;
				}
			}
			else
			{
				if(indata[i] == 0xE8 && indata[i + 5] == 0x60 && indata[i + 6] == 0xAD) {
					depacker_start = i;
				}
			}

			if(indata[i] == 0xBE && indata[i + 3] == 0x40 && indata[i + 4] == 0x00) {
				models_address = *(int*)&indata[i + 1];
			}
		}
	}

	int models_offset = -1;

	int rawsize = 0;
	int splittingPoint = 0;
	if(is_tiny_header)
	{
		if(return_offset == -1 && compressed_data_rva != -1)
		{
			NotCrinklerFileError();
		}

		rawsize = *(unsigned short*)&indata[rawsize_start];
		splittingPoint = rawsize;
	}
	else
	{
		if(hashtable_size == -1 || return_offset == -1 || (depacker_start == -1 && is_compatibility_header) || models_address == -1)
		{
			NotCrinklerFileError();
		}

		models_offset = models_address - CRINKLER_IMAGEBASE;
		unsigned int weightmask1 = *(unsigned int*)&indata[models_offset + 4];
		unsigned char* models1 = &indata[models_offset + 8];
		m_modellist1.SetFromModelsAndMask(models1, weightmask1);
		int modelskip = 8 + m_modellist1.nmodels;
		unsigned int weightmask2 = *(unsigned int*)&indata[models_offset + modelskip + 4];
		unsigned char* models2 = &indata[models_offset + modelskip + 8];
		m_modellist2.SetFromModelsAndMask(models2, weightmask2);

		if(version >= 13) {
			rawsize = -(*(int*)&indata[models_offset + modelskip]) - CRINKLER_CODEBASE;
			splittingPoint = -(*(int*)&indata[models_offset]) - CRINKLER_CODEBASE;
		}
		else {
			rawsize = (*(int*)&indata[models_offset + modelskip]) / 8;
			splittingPoint = (*(int*)&indata[models_offset]) / 8;
		}
	}

	SetUseTinyHeader(is_tiny_header);
	
	CompressionType compmode = m_modellist1.DetectCompressionType();
	int subsystem_version = indata[pe_header_offset+0x5C];
	int large_address_aware = (*(unsigned short *)&indata[pe_header_offset+0x16] & 0x0020) != 0;

	static const unsigned char saturateCode[] = { 0x75, 0x03, 0xFE, 0x0C, 0x1F };
	bool saturate = std::search(indata, indata + length, std::begin(saturateCode), std::end(saturateCode)) != indata + length;
	if (m_saturate == -1) m_saturate = saturate;

	int exports_rva = 0;
	if(!is_tiny_header && majorlv >= '2')
	{
		exports_rva = *(int*)&indata[pe_header_offset + 0x78];
	}
		

	printf("Original file size: %d\n", length);
	printf("Original Tiny Header: %s\n", is_tiny_header ? "YES" : "NO");
	printf("Original Virtual size: %d\n", virtualSize);
	printf("Original Subsystem type: %s\n", subsystem_version == 3 ? "CONSOLE" : "WINDOWS");
	printf("Original Large address aware: %s\n", large_address_aware ? "YES" : "NO");
	if(!is_tiny_header)
	{
		printf("Original Compression mode: %s\n", compmode == COMPRESSION_INSTANT ? "INSTANT" : version < 21 ? "FAST/SLOW" : "FAST/SLOW/VERYSLOW");
		printf("Original Saturate counters: %s\n", saturate ? "YES" : "NO");
		printf("Original Hash size: %d\n", hashtable_size);
	}
	
	if(is_tiny_header)
	{
		printf("Total size: %d\n", rawsize);
		printf("\n");
	}
	else
	{
		printf("Code size: %d\n", splittingPoint);
		printf("Data size: %d\n", rawsize - splittingPoint);
		printf("\n");
	}
	

	STARTUPINFO startupInfo = {0};
	startupInfo.cb = sizeof(startupInfo);

	char tempPath[MAX_PATH];
	GetTempPath(MAX_PATH, tempPath);
	char tempFilename[MAX_PATH];

	GetTempFileName(tempPath, "", 0, tempFilename);
	PROCESS_INFORMATION pi;

	if(!file.Write(tempFilename)) {
		Log::Error("", "Failed to write to temporary file '%s'\n", tempFilename);
	}

	CreateProcess(tempFilename, NULL, NULL, NULL, false, NORMAL_PRIORITY_CLASS|CREATE_SUSPENDED, NULL, NULL, &startupInfo, &pi);
	DebugActiveProcess(pi.dwProcessId);
	ResumeThread(pi.hThread);

	bool done = false;
	do {
		DEBUG_EVENT de;
		if(WaitForDebugEvent(&de, 120000) == 0) {
			Log::Error("", "Program was been unresponsive for more than 120 seconds - closing down\n");
		}

		if(de.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && 
			(de.u.Exception.ExceptionRecord.ExceptionAddress == (PVOID)(size_t)(0x410000+return_offset) ||
			de.u.Exception.ExceptionRecord.ExceptionAddress == (PVOID)(size_t)(0x400000+return_offset)))
		{
			done = true;
		}

		if(!done)
			ContinueDebugEvent(de.dwProcessId, de.dwThreadId, DBG_CONTINUE);
	} while(!done);

	unsigned char* rawdata = new unsigned char[rawsize];
	SIZE_T read;
	if(ReadProcessMemory(pi.hProcess, (LPCVOID)0x420000, rawdata, rawsize, &read) == 0 || read != rawsize) {
		Log::Error("", "Failed to read process memory\n");
	}

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	// Patch calltrans code
	int import_offset = 0;
	if (rawdata[0] == 0x89 && rawdata[1] == 0xD7) { // MOV EDI, EDX
		// Old calltrans code - convert to new
		unsigned int ncalls = rawdata[5];
		rawdata[0] = 0x5F; // POP EDI
		rawdata[1] = 0xB9; // MOV ECX, uint32_t
		*((unsigned int *)&rawdata[2]) = ncalls;
		printf("Call transformation code successfully patched.\n");
		import_offset = 24;
	} else if (rawdata[0] == 0x5F) { // POP EDI
		// New calltrans code
		printf("Call transformation code does not need patching.\n");
		import_offset = 24;
	}

	// Patch import code
	static const unsigned char old_import_code[] = {0x31, 0xC0, 0x64, 0x8B, 0x40, 0x30, 0x8B, 0x40, 
													0x0C, 0x8B, 0x40, 0x1C, 0x8B, 0x40, 0x00, 0x8B,
													0x68, 0x08};
	static const unsigned char new_import_code[] = {0x64, 0x67, 0x8B, 0x47, 0x30, 0x8B, 0x40, 0x0C,
													0x8B, 0x40, 0x0C, 0x8B, 0x00, 0x8B, 0x00, 0x8B,
													0x68, 0x18};
	static const unsigned char new_import_code2[] ={0x58, 0x8B, 0x40, 0x0C, 0x8B, 0x40, 0x0C, 0x8B,
													0x00, 0x8B, 0x00, 0x8B, 0x68, 0x18};
	static const unsigned char tiny_import_code[] ={0x58, 0x8B, 0x40, 0x0C, 0x8B, 0x40, 0x0C, 0x8B,
													0x40, 0x00, 0x8B, 0x40, 0x00, 0x8B, 0x40, 0x18 };
	bool found_import = false;
	int hashes_address = -1;
	int hashes_address_offset = -1;
	int dll_names_address = -1;
	bool is_tiny_import = false;

	for (int i = import_offset ; i < splittingPoint-(int)sizeof(old_import_code) ; i++) {		
		if (rawdata[i] == 0xBB) {
			hashes_address_offset = i + 1;
			hashes_address = *(int*)&rawdata[hashes_address_offset];
		}
		if (rawdata[i] == 0xBE) {
			dll_names_address = *(int*)&rawdata[i + 1];
		}
		if(rawdata[i] == 0xBF) {
			dll_names_address = *(int*)&rawdata[i + 1];
		}

		if (memcmp(rawdata+i, old_import_code, sizeof(old_import_code)) == 0) {			// No calltrans
			memcpy(rawdata+i, new_import_code, sizeof(new_import_code));
			printf("Import code successfully patched.\n");
			found_import = true;
			break;
		}
		if (memcmp(rawdata+i, new_import_code, sizeof(new_import_code)) == 0 || memcmp(rawdata+i, new_import_code2, sizeof(new_import_code2)) == 0)
		{
			printf("Import code does not need patching.\n");
			found_import = true;
			break;
		}
		
		if(memcmp(rawdata + i, tiny_import_code, sizeof(tiny_import_code)) == 0)
		{
			printf("Import code does not need patching.\n");
			found_import = true;
			is_tiny_import = true;
			break;
		}
	}
	
	if(!found_import || dll_names_address == -1)
	{
		Log::Error("", "Cannot find old import code to patch\n");
	}

	// Make the 1k report a little more readable
	if(is_tiny_header && dll_names_address - CRINKLER_CODEBASE < splittingPoint)
	{
		splittingPoint = dll_names_address - CRINKLER_CODEBASE;	
	}

	SetUseTinyImport(is_tiny_import);

	printf("\n");

	if (!m_replaceDlls.empty())
	{
		if(is_tiny_header)
		{
			char* start_ptr = (char*)&rawdata[dll_names_address - CRINKLER_CODEBASE];
			char* end_ptr = (char*)&rawdata[rawsize];
			for(const auto& kv : m_replaceDlls)
			{
				char* pos = std::search(start_ptr, end_ptr, kv.first.begin(), kv.first.end());
				if(pos != end_ptr)
				{
					strcpy(pos, kv.second.c_str());
				}
			}
		}
		else
		{
			char* name = (char*)&rawdata[dll_names_address + 1 - CRINKLER_CODEBASE];
			while(name[0] != (char)0xFF)
			{
				if(m_replaceDlls.count(name))
				{
					assert(m_replaceDlls[name].length() == strlen(name));
					strcpy(name, m_replaceDlls[name].c_str());
				}
				name += strlen(name) + 2;
			}
		}
	}

	HunkList* headerHunks = NULL;
	if(is_tiny_header)
	{
		headerHunks = m_hunkLoader.Load(header1KObj, int(header1KObj_end - header1KObj), "crinkler header");
	}
	else
	{
		if(is_compatibility_header)
		{
			headerHunks = m_hunkLoader.Load(headerCompatibilityObj, int(headerCompatibilityObj_end - headerCompatibilityObj), "crinkler header");
		}
		else
		{
			headerHunks = m_hunkLoader.Load(headerObj, int(headerObj_end - headerObj), "crinkler header");
		}
	}
	
	Hunk* header = headerHunks->FindSymbol("_header")->hunk;
	Hunk* depacker = nullptr;
	
	if (is_compatibility_header) {
		depacker = headerHunks->FindSymbol("_DepackEntry")->hunk;
		SetHeaderSaturation(depacker);
	}

	if(!is_tiny_import)
	{
		int new_hashes_address = is_compatibility_header ? CRINKLER_IMAGEBASE : CRINKLER_IMAGEBASE + header->GetRawSize();
		*(int*)&rawdata[hashes_address_offset] = new_hashes_address;
	}
	

	Hunk* phase1 = new Hunk("linked", (char*)rawdata, HUNK_IS_CODE|HUNK_IS_WRITEABLE, 0, rawsize, virtualSize);
	delete[] rawdata;

	if(!is_tiny_header)
	{
		// Handle exports
		std::set<Export> exports;
		printf("Original Exports:");
		if(exports_rva) {
			exports = StripExports(phase1, exports_rva);
			printf("\n");
			PrintExports(exports);
			if(!m_stripExports) {
				for(const Export& e : exports) {
					AddExport(e);
				}
			}
		}
		else {
			printf(" NONE\n");
		}
		printf("Resulting Exports:");
		if(!m_exports.empty()) {
			printf("\n");
			PrintExports(m_exports);
			for(const Export& e : m_exports) {
				if(!e.HasValue()) {
					Symbol *sym = phase1->FindSymbol(e.GetSymbol().c_str());
					if(!sym) {
						Log::Error("", "Cannot find symbol '%s' to be exported under name '%s'.", e.GetSymbol().c_str(), e.GetName().c_str());
					}
				}
			}

			int padding = exports_rva ? 0 : 16;
			phase1->SetVirtualSize(phase1->GetRawSize() + padding);
			Hunk* export_hunk = CreateExportTable(m_exports);
			HunkList hl;
			hl.AddHunkBack(phase1);
			hl.AddHunkBack(export_hunk);
			Hunk* with_exports = hl.ToHunk("linked", CRINKLER_CODEBASE);
			hl.Clear();
			with_exports->SetVirtualSize(virtualSize);
			with_exports->Relocate(CRINKLER_CODEBASE);
			delete phase1;
			phase1 = with_exports;
		}
		else {
			printf(" NONE\n");
		}
	}
	
	phase1->Trim();

	printf("\nRecompressing...\n");

	int maxsize = phase1->GetRawSize()*2+1000;

	int* sizefill = new int[maxsize];

	unsigned char* data = new unsigned char[maxsize];
	int best_hashsize = 0;
	int size;
	if(is_tiny_header)
	{
		size = Compress1k((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(), data, maxsize, m_modellist1k, sizefill, nullptr);
		printf("Real compressed total size: %d\n", size);
	}
	else
	{
		int idealsize = 0;
		if(m_compressionType < 0)
		{
			// Keep models
			if(m_hashsize < 0) {
				// Use original optimized hash size
				SetHashsize((hashtable_size - 1) / (1024 * 1024) + 1);
				best_hashsize = hashtable_size;
				SetHashtries(0);
			}
			else {
				best_hashsize = PreviousPrime(m_hashsize / 2) * 2;
				InitProgressBar();

				// Rehash
				best_hashsize = OptimizeHashsize((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(), best_hashsize, splittingPoint, m_hashtries);
				DeinitProgressBar();
			}
		}
		else {
			if(m_hashsize < 0) {
				SetHashsize((hashtable_size - 1) / (1024 * 1024) + 1);
			}
			best_hashsize = PreviousPrime(m_hashsize / 2) * 2;
			if(m_compressionType != COMPRESSION_INSTANT) {
				InitProgressBar();
				idealsize = EstimateModels((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(), splittingPoint, false, false, INT_MAX, INT_MAX);

				// Hashing
				best_hashsize = OptimizeHashsize((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(), best_hashsize, splittingPoint, m_hashtries);
				DeinitProgressBar();
			}
		}

		ModelList4k* modelLists[] = { &m_modellist1, &m_modellist2 };
		int segmentSizes[] = { splittingPoint, phase1->GetRawSize() - splittingPoint };
		size = Compress4k((unsigned char*)phase1->GetPtr(), 2, segmentSizes, data, maxsize, modelLists, m_saturate != 0, CRINKLER_BASEPROB, best_hashsize, sizefill);

		if(m_compressionType != -1 && m_compressionType != COMPRESSION_INSTANT) {
			int sizeIncludingModels = size + m_modellist1.nmodels + m_modellist2.nmodels;
			float byteslost = sizeIncludingModels - idealsize / (float)(BIT_PRECISION * 8);
			printf("Real compressed total size: %d\nBytes lost to hashing: %.2f\n", sizeIncludingModels, byteslost);
		}

		SetCompressionType(compmode);
	}

	if(is_compatibility_header)
	{	// Copy hashes from old header
		uint32_t* new_header_ptr = (uint32_t*)header->GetPtr();
		uint32_t* old_header_ptr = (uint32_t*)indata;

		for(int i = 0; i < depacker_start / 4; i++) {
			if(new_header_ptr[i] == 'HSAH')
				new_header_ptr[i] = old_header_ptr[i];
		}
		header->SetRawSize(depacker_start);
		header->SetVirtualSize(depacker_start);
	}

	Hunk *hashHunk = nullptr;
	if (!is_compatibility_header && !is_tiny_import)
	{
		// Create hunk with hashes
		int hashes_offset = hashes_address - CRINKLER_IMAGEBASE;
		int hashes_bytes = is_tiny_header ? (compressed_data_rva - CRINKLER_IMAGEBASE - hashes_offset) : (models_offset - hashes_offset);
		hashHunk = new Hunk("HashHunk", (char*)&indata[hashes_offset], 0, 0, hashes_bytes, hashes_bytes);
	}

	if (m_subsystem >= 0) {
		subsystem_version = (m_subsystem == SUBSYSTEM_WINDOWS) ? IMAGE_SUBSYSTEM_WINDOWS_GUI : IMAGE_SUBSYSTEM_WINDOWS_CUI;
	}
	if (m_largeAddressAware == -1) {
		m_largeAddressAware = large_address_aware;
	}
	SetSubsystem((subsystem_version == IMAGE_SUBSYSTEM_WINDOWS_GUI) ? SUBSYSTEM_WINDOWS : SUBSYSTEM_CONSOLE);

	Hunk *phase2 = FinalLink(header, depacker, hashHunk, phase1, data, size, splittingPoint, best_hashsize);
	delete[] data;

	CompressionReportRecord* csr = phase1->GetCompressionSummary(sizefill, splittingPoint);
	if(m_printFlags & PRINT_LABELS)
		VerboseLabels(csr);
	if(!m_summaryFilename.empty())
		HtmlReport(csr, m_summaryFilename.c_str(), *phase1, *phase1, sizefill,
			output_filename, phase2->GetRawSize(), this);
	delete csr;
	delete[] sizefill;

	if (!outfile) {
		if(fopen_s(&outfile, output_filename, "wb")) {
			Log::Error("", "Cannot open '%s' for writing", output_filename);
			return;
		}
	}
	fwrite(phase2->GetPtr(), 1, phase2->GetRawSize(), outfile);
	fclose(outfile);

	printf("\nOutput file: %s\n", output_filename);
	printf("Final file size: %d\n\n", phase2->GetRawSize());

	delete phase1;
	delete phase2;
#endif
}

Hunk* Crinkler::CreateDynamicInitializerHunk()
{
	const int num_hunks = m_hunkPool.GetNumHunks();
	std::vector<Symbol*> symbols;
	for(int i = 0; i < num_hunks; i++)
	{
		Hunk* hunk = m_hunkPool[i];
		if(EndsWith(hunk->GetName(), "CRT$XCU"))
		{
			int num_relocations = hunk->GetNumRelocations();
			Relocation* relocations = hunk->GetRelocations();
			for(int i = 0; i < num_relocations; i++)
			{
				symbols.push_back(m_hunkPool.FindSymbol(relocations[i].symbolname.c_str()));
			}
		}
	}

	if(!symbols.empty())
	{
		const int num_symbols = (int)symbols.size();
		const int hunk_size = num_symbols*5;
		Hunk* hunk = new Hunk("dynamic initializer calls", NULL, HUNK_IS_CODE, 0, hunk_size, hunk_size);

		char* ptr = hunk->GetPtr();
		for(int i = 0; i < num_symbols; i++)
		{
			*ptr++ = (char)0xE8;
			*ptr++ = 0x00;
			*ptr++ = 0x00;
			*ptr++ = 0x00;
			*ptr++ = 0x00;
			
			Relocation r;
			r.offset = i*5+1;
			r.symbolname = symbols[i]->name;
			r.type = RELOCTYPE_REL32;
			hunk->AddRelocation(r);
		}
		hunk->AddSymbol(new Symbol("__DynamicInitializers", 0, SYMBOL_IS_RELOCATEABLE, hunk));
		printf("\nIncluded %d dynamic initializer%s.\n", num_symbols, num_symbols == 1 ? "" : "s");
		return hunk;
	}
	return NULL;
}

void Crinkler::Link(const char* filename) {
	// Open output file immediate, just to be sure
	FILE* outfile;
	int old_filesize = 0;
	if (!fopen_s(&outfile, filename, "rb")) {
		// Find old size
		fseek(outfile, 0, SEEK_END);
		old_filesize = ftell(outfile);
		fclose(outfile);
	}
	if(fopen_s(&outfile, filename, "wb")) {
		Log::Error("", "Cannot open '%s' for writing", filename);
		return;
	}


	// Find entry hunk and move it to front
	Symbol* entry = FindEntryPoint();
	if(entry == NULL)
		return;

	Hunk* dynamicInitializersHunk = NULL;
	if (m_runInitializers) {
		dynamicInitializersHunk = CreateDynamicInitializerHunk();
		if(dynamicInitializersHunk)
		{
			m_hunkPool.AddHunkBack(dynamicInitializersHunk);
		}
	}

	// Color hunks from entry hunk
	RemoveUnreferencedHunks(entry->hunk);

	// Replace DLLs
	ReplaceDlls(m_hunkPool);

	if (m_overrideAlignments) OverrideAlignments(m_hunkPool);

	// 1-byte align entry point and other sections
	int n_unaligned = 0;
	bool entry_point_unaligned = false;
	if(entry->hunk->GetAlignmentBits() > 0) {
		entry->hunk->SetAlignmentBits(0);
		n_unaligned++;
		entry_point_unaligned = true;
	}
	if (m_unalignCode) {
		for (int i = 0; i < m_hunkPool.GetNumHunks(); i++) {
			Hunk* hunk = m_hunkPool[i];
			if (hunk->GetFlags() & HUNK_IS_CODE && !(hunk->GetFlags() & HUNK_IS_ALIGNED) && hunk->GetAlignmentBits() > 0) {
				hunk->SetAlignmentBits(0);
				n_unaligned++;
			}
		}
	}
	if (n_unaligned > 0) {
		printf("Forced alignment of %d code hunk%s to 1", n_unaligned, n_unaligned > 1 ? "s" : "");
		if (entry_point_unaligned) {
			printf(" (including entry point)");
		}
		printf(".\n");
	}

	// Load appropriate header
	HunkList* headerHunks = m_useTinyHeader ?	m_hunkLoader.Load(header1KObj, int(header1KObj_end - header1KObj), "crinkler header") :
												m_hunkLoader.Load(headerObj, int(headerObj_end - headerObj), "crinkler header");

	Hunk* header = headerHunks->FindSymbol("_header")->hunk;
	if(!m_useTinyHeader)
		SetHeaderSaturation(header);
	Hunk* hashHunk = NULL;

	int hash_bits;
	int max_dll_name_length;
	bool usesRangeImport=false;
	{	// Add imports
		HunkList* importHunkList = m_useTinyImport ? ImportHandler::CreateImportHunks1K(&m_hunkPool, (m_printFlags & PRINT_IMPORTS) != 0, hash_bits, max_dll_name_length) :
													ImportHandler::CreateImportHunks(&m_hunkPool, hashHunk, m_fallbackDlls, m_rangeDlls, (m_printFlags & PRINT_IMPORTS) != 0, usesRangeImport);
		m_hunkPool.RemoveImportHunks();
		m_hunkPool.Append(importHunkList);
		delete importHunkList;
	}

	LoadImportCode(m_useTinyImport, m_useSafeImporting, !m_fallbackDlls.empty(), usesRangeImport);

	Symbol* importSymbol = m_hunkPool.FindSymbol("_Import");

	if(dynamicInitializersHunk)
	{		
		m_hunkPool.RemoveHunk(dynamicInitializersHunk);
		m_hunkPool.AddHunkFront(dynamicInitializersHunk);
		dynamicInitializersHunk->SetContinuation(entry);
	}

	Hunk* importHunk = importSymbol->hunk;

	m_hunkPool.RemoveHunk(importHunk);
	m_hunkPool.AddHunkFront(importHunk);
	importHunk->SetAlignmentBits(0);
	importHunk->SetContinuation(dynamicInitializersHunk ? dynamicInitializersHunk->FindSymbol("__DynamicInitializers") : entry);
	
	// Make sure import and startup code has access to the _ImageBase address
	importHunk->AddSymbol(new Symbol("_ImageBase", CRINKLER_IMAGEBASE, 0, importHunk));
	importHunk->AddSymbol(new Symbol("___ImageBase", CRINKLER_IMAGEBASE, 0, importHunk));

	if(m_useTinyImport)
	{
		*(importHunk->GetPtr() + importHunk->FindSymbol("_HashShiftPtr")->value) = 32 - hash_bits;
		*(importHunk->GetPtr() + importHunk->FindSymbol("_MaxNameLengthPtr")->value) = max_dll_name_length;
	}

	// Truncate floats
	if(m_truncateFloats) {
		printf("\nTruncating floats:\n");
		m_hunkPool.RoundFloats(m_truncateBits);
	}

	if (!m_exports.empty()) {
		m_hunkPool.AddHunkBack(CreateExportTable(m_exports));
	}

	// Sort hunks heuristically
	HeuristicHunkSorter::SortHunkList(&m_hunkPool);

	int best_hashsize = PreviousPrime(m_hashsize / 2) * 2;

	Reuse *reuse = nullptr;
	int reuse_filesize = 0;
	ReuseType reuseType = m_useTinyHeader ? REUSE_OFF : m_reuseType;
	if (reuseType != REUSE_OFF && reuseType != REUSE_WRITE) {
		reuse = LoadReuseFile(m_reuseFilename.c_str());
		if (reuse != nullptr) {
			m_modellist1 = *reuse->GetCodeModels();
			m_modellist2 = *reuse->GetDataModels();
			ExplicitHunkSorter::SortHunkList(&m_hunkPool, reuse);
			best_hashsize = reuse->GetHashSize();
			printf("\nRead reuse file: %s\n", m_reuseFilename.c_str());
		}
	}

	// Create phase 1 data hunk
	int splittingPoint;
	Hunk* phase1, *phase1Untransformed;
	m_hunkPool[0]->AddSymbol(new Symbol("_HeaderHashes", CRINKLER_IMAGEBASE+header->GetRawSize(), SYMBOL_IS_SECTION, m_hunkPool[0]));

	if (!m_transform->LinkAndTransform(&m_hunkPool, importSymbol, CRINKLER_CODEBASE, phase1, &phase1Untransformed, &splittingPoint, true))
	{
		// Transform failed, run again
		delete phase1;
		delete phase1Untransformed;
		m_transform->LinkAndTransform(&m_hunkPool, importSymbol, CRINKLER_CODEBASE, phase1, &phase1Untransformed, &splittingPoint, false);
	}
	int maxsize = phase1->GetRawSize()*2+1000;	// Allocate plenty of memory	
	unsigned char* data = new unsigned char[maxsize];

	if (reuseType == REUSE_IMPROVE && reuse != nullptr) {
		ModelList4k* modelLists[] = { &m_modellist1, &m_modellist2 };
		int segmentSizes[] = { splittingPoint, phase1->GetRawSize()- splittingPoint };
		int size = Compress4k((unsigned char*)phase1->GetPtr(), 2, segmentSizes, data, maxsize, modelLists, m_saturate != 0, CRINKLER_BASEPROB, best_hashsize, nullptr);
		Hunk *phase2 = FinalLink(header, nullptr, hashHunk, phase1, data, size, splittingPoint, best_hashsize);
		reuse_filesize = phase2->GetRawSize();
		delete phase2;

		printf("\nFile size with reuse parameters: %d\n", reuse_filesize);
	}

	printf("\nUncompressed size of code: %5d\n", splittingPoint);
	printf("Uncompressed size of data: %5d\n", phase1->GetRawSize() - splittingPoint);

	int* sizefill = new int[maxsize];
	int size, idealsize = 0;
	if (m_useTinyHeader || m_compressionType != COMPRESSION_INSTANT)
	{
		if (reuseType == REUSE_STABLE && reuse != nullptr) {
			// Calculate ideal size with reuse parameters
			ModelList4k* modelLists[] = { &m_modellist1, &m_modellist2 };
			int segmentSizes[] = { splittingPoint, phase1->GetRawSize() - splittingPoint};
			int compressedSizes[2] = {};
			
			idealsize = EvaluateSize4k((unsigned char*)phase1->GetPtr(), 2, segmentSizes, compressedSizes, modelLists, CRINKLER_BASEPROB, m_saturate != 0);
			printf("\nIdeal compressed size of code: %.2f\n", compressedSizes[0] / (float)(BIT_PRECISION * 8));
			printf("Ideal compressed size of data: %.2f\n", compressedSizes[1] / (float)(BIT_PRECISION * 8));
			printf("Ideal compressed total size: %.2f\n", idealsize / (float)(BIT_PRECISION * 8));
		}
		else {
			// Full size estimation and hunk reordering
			bool verbose_models = (m_printFlags & PRINT_MODELS) != 0;
			InitProgressBar();

			idealsize = EstimateModels((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(), splittingPoint, false, m_useTinyHeader, INT_MAX, INT_MAX);

			if (m_hunktries > 0)
			{
				int target_size1, target_size2;
				EmpiricalHunkSorter::SortHunkList(&m_hunkPool, *m_transform, m_modellist1, m_modellist2, m_modellist1k, CRINKLER_BASEPROB, m_saturate != 0, m_hunktries,
#ifdef WIN32
						m_showProgressBar ? &m_windowBar :
#endif
						NULL,
						m_useTinyHeader, &target_size1, &target_size2);
				delete phase1;
				delete phase1Untransformed;
				m_transform->LinkAndTransform(&m_hunkPool, importSymbol, CRINKLER_CODEBASE, phase1, &phase1Untransformed, &splittingPoint, true);

				idealsize = EstimateModels((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(), splittingPoint, true, m_useTinyHeader, target_size1, target_size2);
			}

			// Hashing time
			if (!m_useTinyHeader)
			{
				best_hashsize = PreviousPrime(m_hashsize / 2) * 2;
				best_hashsize = OptimizeHashsize((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(), best_hashsize, splittingPoint, m_hashtries);
			}

			DeinitProgressBar();
		}
	}

	if (m_useTinyHeader)
	{
		size = Compress1k((unsigned char*)phase1->GetPtr(), phase1->GetRawSize(),data, maxsize, m_modellist1k, sizefill, nullptr);
	}
	else
	{
		ModelList4k* modelLists[] = { &m_modellist1, &m_modellist2 };
		int segmentSizes[] = { splittingPoint, phase1->GetRawSize() - splittingPoint };
		size = Compress4k((unsigned char*)phase1->GetPtr(), 2, segmentSizes, data, maxsize, modelLists, m_saturate != 0, CRINKLER_BASEPROB, best_hashsize, sizefill);
	}
	
	if(!m_useTinyHeader && m_compressionType != COMPRESSION_INSTANT) {
		int sizeIncludingModels = size + m_modellist1.nmodels + m_modellist2.nmodels;
		float byteslost = sizeIncludingModels - idealsize / (float) (BIT_PRECISION * 8);
		printf("Real compressed total size: %d\nBytes lost to hashing: %.2f\n", sizeIncludingModels, byteslost);
	}

	Hunk *phase2 = FinalLink(header, nullptr, hashHunk, phase1, data, size, splittingPoint, best_hashsize);
	delete[] data;

	CompressionReportRecord* csr = phase1->GetCompressionSummary(sizefill, splittingPoint);
	if(m_printFlags & PRINT_LABELS)
		VerboseLabels(csr);
	if(!m_summaryFilename.empty())
		HtmlReport(csr, m_summaryFilename.c_str(), *phase1, *phase1Untransformed, sizefill,
			filename, phase2->GetRawSize(), this);
	delete csr;
	delete[] sizefill;
	
	fwrite(phase2->GetPtr(), 1, phase2->GetRawSize(), outfile);
	fclose(outfile);

	printf("\nOutput file: %s\n", filename);
	printf("Final file size: %d", phase2->GetRawSize());
	if (old_filesize)
	{
		if (old_filesize != phase2->GetRawSize()) {
			printf(" (previous size %d)", old_filesize);
		} else {
			printf(" (no change)");
		}
	}
	printf("\n\n");

	if (reuseType != REUSE_OFF) {
		bool write = false;
		if (reuse == nullptr) {
			printf("Writing reuse file: %s\n\n", m_reuseFilename.c_str());
			write = true;
		}
		else if (reuseType == REUSE_IMPROVE) {
			if (phase2->GetRawSize() < reuse_filesize) {
				printf("Overwriting reuse file: %s\n\n", m_reuseFilename.c_str());
				write = true;
				delete reuse;
			}
			else {
				printf("Size not better than with reuse parameters - keeping reuse file: %s\n\n", m_reuseFilename.c_str());
			}
		}
		if (write) {
			reuse = new Reuse(m_modellist1, m_modellist2, m_hunkPool, best_hashsize);
			reuse->Save(m_reuseFilename.c_str());
		}
	}

	if (phase2->GetRawSize() > 128*1024)
	{
		Log::Error(filename, "Output file too big. Crinkler does not support final file sizes of more than 128k.");
	}

	if (reuse) delete reuse;
	delete phase1;
	delete phase1Untransformed;
	delete phase2;
}

Hunk *Crinkler::FinalLink(Hunk *header, Hunk *depacker, Hunk *hashHunk,
	Hunk *phase1, unsigned char *data, int size, int splittingPoint, int hashsize)
{
	Hunk* phase1Compressed = new Hunk("compressed data", (char*)data, 0, 0, size, size);
	phase1Compressed->AddSymbol(new Symbol("_PackedData", 0, SYMBOL_IS_RELOCATEABLE, phase1Compressed));

	Hunk *modelHunk = nullptr;
	if (!m_useTinyHeader)
	{
		header->AddSymbol(new Symbol("_HashTable", CRINKLER_SECTIONSIZE * 2 + phase1->GetRawSize(), SYMBOL_IS_RELOCATEABLE, header));
		modelHunk = CreateModelHunk(splittingPoint, phase1->GetRawSize());
	}

	HunkList phase2list;
	phase2list.AddHunkBack(new Hunk(*header));
	if (depacker) phase2list.AddHunkBack(new Hunk(*depacker));
	if (hashHunk) phase2list.AddHunkBack(new Hunk(*hashHunk));
	if (modelHunk) phase2list.AddHunkBack(modelHunk);
	phase2list.AddHunkBack(phase1Compressed);
	Hunk* phase2 = phase2list.ToHunk("final", CRINKLER_IMAGEBASE);

	// Add constants
	int exports_rva = m_useTinyHeader || m_exports.empty() ? 0 : phase1->FindSymbol("_ExportTable")->value + CRINKLER_CODEBASE - CRINKLER_IMAGEBASE;
	SetHeaderConstants(phase2, phase1, hashsize, m_modellist1k.boost, m_modellist1k.baseprob0, m_modellist1k.baseprob1, m_modellist1k.modelmask, m_subsystem == SUBSYSTEM_WINDOWS ? IMAGE_SUBSYSTEM_WINDOWS_GUI : IMAGE_SUBSYSTEM_WINDOWS_CUI, exports_rva, m_useTinyHeader);
	phase2->Relocate(CRINKLER_IMAGEBASE);

	return phase2;
}

void Crinkler::PrintOptions(FILE *out) {
	fprintf(out, " /SUBSYSTEM:%s", m_subsystem == SUBSYSTEM_CONSOLE ? "CONSOLE" : "WINDOWS");
	if (m_largeAddressAware) {
		fprintf(out, " /LARGEADDRESSAWARE");
	}
	if (!m_entry.empty()) {
		fprintf(out, " /ENTRY:%s", m_entry.c_str());
	}
	if(m_useTinyHeader) {
		fprintf(out, " /TINYHEADER");
	}
	if(m_useTinyImport) {
		fprintf(out, " /TINYIMPORT");
	}
	
	if(!m_useTinyHeader)
	{
		fprintf(out, " /COMPMODE:%s", CompressionTypeName(m_compressionType));
		if (m_saturate) {
			fprintf(out, " /SATURATE");
		}
		fprintf(out, " /HASHSIZE:%d", m_hashsize / 1048576);
	}
	
	if (m_compressionType != COMPRESSION_INSTANT) {
		if(!m_useTinyHeader)
		{
			fprintf(out, " /HASHTRIES:%d", m_hashtries);
		}
		fprintf(out, " /ORDERTRIES:%d", m_hunktries);
	}
	for(int i = 0; i < (int)m_rangeDlls.size(); i++) {
		fprintf(out, " /RANGE:%s", m_rangeDlls[i].c_str());
	}
	for(const auto& p : m_replaceDlls) {
		fprintf(out, " /REPLACEDLL:%s=%s", p.first.c_str(), p.second.c_str());
	}
	for (const auto& p : m_fallbackDlls) {
		fprintf(out, " /FALLBACKDLL:%s=%s", p.first.c_str(), p.second.c_str());
	}
	if (!m_useTinyHeader && !m_useSafeImporting) {
		fprintf(out, " /UNSAFEIMPORT");
	}
	if (m_transform->GetDetransformer() != NULL) {
		fprintf(out, " /TRANSFORM:CALLS");
	}
	if (m_truncateFloats) {
		fprintf(out, " /TRUNCATEFLOATS:%d", m_truncateBits);
	}
	if (m_overrideAlignments) {
		fprintf(out, " /OVERRIDEALIGNMENTS");
		if (m_alignmentBits != -1) {
			fprintf(out, ":%d", m_alignmentBits);
		}
	}
	if (m_unalignCode) {
		fprintf(out, " /UNALIGNCODE");
	}
	if (!m_runInitializers) {
		fprintf(out, " /NOINITIALIZERS");
	}
	for (const Export& e : m_exports) {
		if (e.HasValue()) {
			fprintf(out, " /EXPORT:%s=0x%08X", e.GetName().c_str(), e.GetValue());
		} else if (e.GetName() == e.GetSymbol()) {
			fprintf(out, " /EXPORT:%s", e.GetName().c_str());
		} else {
			fprintf(out, " /EXPORT:%s=%s", e.GetName().c_str(), e.GetSymbol().c_str());
		}
	}
}

void Crinkler::InitProgressBar() {
	m_progressBar.AddProgressBar(&m_consoleBar);
#ifdef WIN32
	if(m_showProgressBar)
		m_progressBar.AddProgressBar(&m_windowBar);
#endif
	m_progressBar.Init();
}

void Crinkler::DeinitProgressBar() {
	m_progressBar.Deinit();
}
