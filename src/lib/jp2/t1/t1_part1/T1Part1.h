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
#include "stdint.h"
#include "testing.h"
#include "TileProcessor.h"
#include "T1Interface.h"
#include "t1_common.h"
#include "T1.h"



namespace grk {

namespace t1_part1 {

class T1Part1: public T1Interface {
public:
	T1Part1(bool isEncoder, uint32_t maxCblkW, uint32_t maxCblkH);
	virtual ~T1Part1();

	void preEncode(EncodeBlockInfo *block, grk_tile *tile, uint32_t &max);
	double compress(EncodeBlockInfo *block, grk_tile *tile, uint32_t max,
			bool doRateControl);

	bool decompress(DecodeBlockInfo *block);
	bool postDecode(DecodeBlockInfo *block);

private:
	T1 *t1;
};
}
}
