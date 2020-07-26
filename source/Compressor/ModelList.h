#pragma once
#ifndef _MODEL_LIST_
#define _MODEL_LIST_

#include <cstdio>

class Compressor;
enum CompressionType {COMPRESSION_INSTANT, COMPRESSION_FAST, COMPRESSION_SLOW, COMPRESSION_VERYSLOW};

static const int MAX_MODELS = 256;

struct Model {
	unsigned char weight;
	unsigned char mask;
};

class ModelList4k {
	Model	m_models[MAX_MODELS];
public:
	int		nmodels;
	int		size;

	ModelList4k();
	ModelList4k(const unsigned char* models, int weightmask);
	ModelList4k(const ModelList4k& ml);
	
	ModelList4k&	operator=(const ModelList4k& ml);
	Model&			operator[] (unsigned idx);
	const Model&	operator[] (unsigned idx) const;

	void			AddModel(Model model);
	void			SetFromModelsAndMask(const unsigned char* models, int weightmask);
	void			Print(FILE *f) const;
	unsigned int	GetMaskList(unsigned char* masks, bool terminate) const;
	CompressionType DetectCompressionType() const;
};

class ModelList1k
{
public:
	unsigned int modelmask;
	unsigned int boost;
	unsigned int baseprob0;
	unsigned int baseprob1;

	void Print() const;
};

#endif
