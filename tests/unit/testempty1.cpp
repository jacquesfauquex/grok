/*
*    Copyright (C) 2016-2018 Grok Image Compression Inc.
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
*
*    This source code incorporates work covered by the following copyright and
*    permission notice:
*
 * Copyright (c) 2012, Mathieu Malaterre
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
extern "C" {
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "opj_config.h"
#include "openjpeg.h"
}

#define J2K_CFMT 0

void error_callback(const char *msg, void *v);
void warning_callback(const char *msg, void *v);
void info_callback(const char *msg, void *v);

void error_callback(const char *msg, void *v)
{
    (void)msg;
    (void)v;
    puts(msg);
}
void warning_callback(const char *msg, void *v)
{
    (void)msg;
    (void)v;
    puts(msg);
}
void info_callback(const char *msg, void *v)
{
    (void)msg;
    (void)v;
    puts(msg);
}

int main(int argc, char *argv[])
{
    const char * v = opj_version();

    const OPJ_COLOR_SPACE color_space = OPJ_CLRSPC_GRAY;
    unsigned int numcomps = 1;
    unsigned int i;
    unsigned int image_width = 256;
    unsigned int image_height = 256;

    opj_cparameters_t parameters;

    unsigned int subsampling_dx = 0;
    unsigned int subsampling_dy = 0;

    opj_image_cmptparm_t cmptparm;
    opj_image_t *image;
    opj_codec_t* l_codec = nullptr;
    bool bSuccess;
    opj_stream_t *l_stream = nullptr;
    (void)argc;
    (void)argv;

    opj_set_default_encoder_parameters(&parameters);
    parameters.cod_format = J2K_CFMT;
    puts(v);
    cmptparm.prec = 8;
    cmptparm.sgnd = 0;
    cmptparm.dx = subsampling_dx;
    cmptparm.dy = subsampling_dy;
    cmptparm.w = image_width;
    cmptparm.h = image_height;

    image = opj_image_create(numcomps, &cmptparm, color_space);
    assert( image );

    for (i = 0; i < image_width * image_height; i++) {
        unsigned int compno;
        for(compno = 0; compno < numcomps; compno++) {
            image->comps[compno].data[i] = 0;
        }
    }

    /* catch events using our callbacks and give a local context */
    opj_set_info_handler(l_codec, info_callback,nullptr);
    opj_set_warning_handler(l_codec, warning_callback,nullptr);
    opj_set_error_handler(l_codec, error_callback,nullptr);

    l_codec = opj_create_compress(OPJ_CODEC_J2K);
    opj_set_info_handler(l_codec, info_callback,nullptr);
    opj_set_warning_handler(l_codec, warning_callback,nullptr);
    opj_set_error_handler(l_codec, error_callback,nullptr);

    opj_setup_encoder(l_codec, &parameters, image);

    l_stream = opj_stream_create_default_file_stream("testempty1.j2k",false);
    assert(l_stream);
    bSuccess = opj_start_compress(l_codec,image,l_stream);
    if( !bSuccess ) {
        opj_stream_destroy(l_stream);
        opj_destroy_codec(l_codec);
        opj_image_destroy(image);
        return 0;
    }

    assert( bSuccess );
    bSuccess = opj_encode(l_codec,l_stream);
    assert( bSuccess );
    bSuccess = opj_end_compress(l_codec, l_stream);
    assert( bSuccess );

    opj_stream_destroy(l_stream);

    opj_destroy_codec(l_codec);
    opj_image_destroy(image);

    puts( "end" );
    return 0;
}
