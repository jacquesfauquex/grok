/*
 *    Copyright (C) 2016-2023 Grok Image Compression Inc.
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
#include <cstdio>
#include <string>
#include <cstring>

#include "grok.h"

void errorCallback(const char* msg, [[maybe_unused]] void* client_data)
{
	auto t = std::string(msg) + "\n";
	fprintf(stderr,t.c_str());
}
void warningCallback(const char* msg, [[maybe_unused]] void* client_data)
{
	auto t = std::string(msg) + "\n";
	fprintf(stdout,t.c_str());
}
void infoCallback(const char* msg, [[maybe_unused]] void* client_data)
{
	auto t = std::string(msg) + "\n";
	fprintf(stdout,t.c_str());
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    const uint32_t dimX = 640;
    const uint32_t dimY = 480;
    const uint32_t numComps = 3;
    const uint32_t precision = 8;
    const char *outFile = "test.jp2";

    uint64_t compressedLength = 0;

    // initialize compress parameters
	grk_cparameters compressParams;
    grk_compress_set_default_params(&compressParams);
    compressParams.cod_format = GRK_FMT_JP2;
    compressParams.verbose = true;

	grk_codec *codec = nullptr;
	grk_image *image = nullptr;
	grk_image_comp* components = nullptr;
	int32_t rc = EXIT_FAILURE;

	bool outputToBuffer = true;

	// initialize library
	grk_initialize(nullptr, 0, false);

	grk_stream_params streamParams;
	memset(&streamParams,0,sizeof(streamParams));
	if (outputToBuffer) {
        streamParams.buf_len = (size_t)numComps * ((precision + 7)/8) * dimX * dimY;
	    streamParams.buf = new uint8_t[streamParams.buf_len];
	} else {
	    streamParams.file = outFile;
	}

	// set library message handlers
	grk_set_msg_handlers(infoCallback, nullptr, warningCallback, nullptr,
						 errorCallback, nullptr);

	// create blank image
	components = new grk_image_comp[numComps];
	for (uint32_t i = 0; i < numComps; ++i){
	    auto c = components + i;
	    c->w = dimX;
	    c->h = dimY;
	    c->dx = 1;
	    c->dy = 1;
	    c->prec = precision;
	    c->sgnd = false;
	}
	image = grk_image_new(numComps, components, GRK_CLRSPC_SRGB, true);

	// fill in component data
    // see grok.h header for full details of image structure
    for (uint16_t compno = 0; compno < image->numcomps; ++compno){
        auto comp = image->comps + compno;
        auto compWidth = comp->w;
        auto compHeight = comp->h;
        auto compData = comp->data;
        if (!compData){
            fprintf(stderr, "Image has null data for component %d\n",compno);
            goto beach;
        }
        // fill in component data, taking component stride into account
        auto srcData = new int32_t[compWidth * compHeight];
        auto srcPtr = srcData;
        for (uint32_t j = 0; j < compHeight; ++j) {
           memcpy(compData, srcPtr, compWidth * sizeof(int32_t));
           srcPtr += compWidth;
           compData += comp->stride;
        }
        delete[] srcData;
    }

	// initialize compressor
	codec = grk_compress_init(&streamParams, &compressParams, image);
	if(!codec)
	{
		fprintf(stderr, "Failed to initialize compressor\n");
		goto beach;
	}

	// compress
	compressedLength = grk_compress(codec, nullptr);
    if (compressedLength == 0)
    {
        fprintf(stderr, "Failed to compress\n");
        goto beach;
    }

    printf("Compression succeeded: %ld bytes used.\n",compressedLength);

    // write buffer to file
    if (outputToBuffer) {
        auto fp = fopen(outFile, "wb");
        if(!fp)
        {
            fprintf(stderr,"Buffer compress: failed to open file %s for writing", outFile);
        }
        else
        {
            size_t written = fwrite(streamParams.buf, 1, compressedLength, fp);
            if(written != compressedLength)
            {
                fprintf(stderr,"Buffer compress: only %ld bytes written out of %ld total", compressedLength,
                              written);
            }
            fclose(fp);
        }
    }

	rc = EXIT_SUCCESS;
beach:
    // cleanup
    delete[] components;
    delete[] streamParams.buf;
	grk_object_unref(codec);
	grk_object_unref(&image->obj);
    grk_deinitialize();

	return rc;
}
