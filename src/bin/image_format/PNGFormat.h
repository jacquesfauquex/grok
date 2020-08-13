/*
*    Copyright (C) 2016 Grok Image Compression Inc.
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

#include "ImageFormat.h"


void pngSetVerboseFlag(bool verbose);

class PNGFormat : public ImageFormat {
public:
	bool encode(grk_image *  image, const std::string &filename, uint32_t compressionParam) override;
	bool finish_encode(void) override;
	grk_image *  decode(const std::string &filename,  grk_cparameters  *parameters) override;
};

