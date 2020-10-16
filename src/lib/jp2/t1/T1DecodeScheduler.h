/*
 *    Copyright (C) 2016-2020 Grok Image Compression Inc.
 *
 *    This source code is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This source code is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "grk_includes.h"

namespace grk {

struct DecodeBlockInfo;
class T1Interface;

class T1DecodeScheduler {
public:
	T1DecodeScheduler(TileCodingParams *tcp, uint16_t blockw, uint16_t blockh);
	~T1DecodeScheduler();
	bool decompress(std::vector<DecodeBlockInfo*> *blocks);

private:
	bool decompressBlock(T1Interface *impl, DecodeBlockInfo *block);
	uint16_t codeblock_width, codeblock_height;  //nominal dimensions of block
	std::vector<T1Interface*> threadStructs;
	std::atomic_bool success;

	DecodeBlockInfo** decodeBlocks;
};

}
