//
// PNG Encoder
//
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 6/27/2021
// Original PNG code written 20+ years ago :)
// The goal of this code is to decode PNG images on embedded systems
//
// Copyright 2021 BitBank Software, Inc. All Rights Reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//===========================================================================
//
#ifndef __PNGENC__
#define __PNGENC__
#if defined( __MACH__ ) || defined( __LINUX__ ) || defined( __MCUXPRESSO ) || defined(ESP_PLATFORM)
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#define memcpy_P memcpy
#define PROGMEM
#else
#include <Arduino.h>
#endif

//
// PNG Encoder
// Written by Larry Bank
// Copyright (c) 2021 BitBank Software, Inc.
// 
// Designed to encode PNG files from source images (1-32 bpp)
// using less than 100K of RAM
//
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
// Number of bits to reduce the zlib window size
// 0 = 256K + 6K needed
// 1 = 128K + 6K needed
// 2 = 64K ...
#if defined( __LINUX__ ) || defined (__MACH__)
#define MEM_SHRINK 0
#else
#define MEM_SHRINK 3
#endif

/* Defines and variables */
#define PNG_FILE_BUF_SIZE 2048
#define PNG_FILE_HIGHWATER ((PNG_FILE_BUF_SIZE * 3)/4)
// Number of bytes to reserve for current and previous lines
// Defaults to 640 32-bit pixels max width
#ifndef PNG_MAX_BUFFERED_PIXELS
#define PNG_MAX_BUFFERED_PIXELS (1024*4 + 1)
#endif


// source pixel type
enum {
  PNG_PIXEL_GRAYSCALE=0,
    PNG_PIXEL_TRUECOLOR=2,
    PNG_PIXEL_INDEXED=3,
    PNG_PIXEL_GRAY_ALPHA=4,
    PNG_PIXEL_TRUECOLOR_ALPHA=6
};

// Error codes returned by getLastError()
enum {
    PNG_SUCCESS = 0,
    PNG_INVALID_PARAMETER,
    PNG_ENCODE_ERROR,
    PNG_MEM_ERROR,
    PNG_NO_BUFFER,
    PNG_UNSUPPORTED_FEATURE,
    PNG_INVALID_FILE,
    PNG_TOO_BIG,
    PNG_NOT_INITIALIZED
};


#ifdef __cplusplus
#define PNG_STATIC static
//
// The PNGENC class wraps portable C code which does the actual work
//
class PNGenc
{
  public:
    PNGenc();
    ~PNGenc();
    int open(uint8_t *pOutput, int iBufferSize);
    int close();
    int encodeBegin(int iWidth, int iHeight, uint8_t iPixelType, uint8_t iBpp, uint8_t *pPalette, uint8_t iCompLevel);
    int addLine(uint8_t *pPixels);
    int addRGB565Line(uint16_t *pPixels, void *pTempLine, bool bBigEndian = false);
    int setTransparentColor(uint32_t u32Color);
    int setAlphaPalette(uint8_t *pPalette);
    int getLastError();

  private:
    struct PNGENCIMAGE *_png;
};
#else
#define PNG_STATIC
int PNG_openRAM(PNGENCIMAGE *pPNG, uint8_t *pData, int iDataSize);
int PNG_close(PNGENCIMAGE *pPNG);
int PNG_encodeBegin(PNGENCIMAGE *pPNG, int iWidth, int iHeight, uint8_t ucPixelType, uint8_t ucBpp, uint8_t *pPalette, uint8_t ucCompLevel);
void PNG_encodeEnd(PNGENCIMAGE *pPNG);
int PNG_addLine(PNGIMAGE *, uint8_t *pPixels, int y);
int PNG_addRGB565Line(PNGENCIMAGE *, uint16_t *pPixels, void *pTempLine, int y);
int PNG_setTransparentColor(PNGENCIMAGE *pPNG, uint32_t u32Color);
int PNG_setAlphaPalette(PNGENCIMAGE *pPNG, uint8_t *pPalette);
int PNG_getLastError(PNGENCIMAGE *pPNG);
#endif // __cplusplus

// Due to unaligned memory causing an exception, we have to do these macros the slow way
#ifndef MOTOLONG
#define INTELSHORT(p) ((*p) + (*(p+1)<<8))
#define INTELLONG(p) ((*p) + (*(p+1)<<8) + (*(p+2)<<16) + (*(p+3)<<24))
#define MOTOSHORT(p) (((*(p))<<8) + (*(p+1)))
#define MOTOLONG(p) (((*p)<<24) + ((*(p+1))<<16) + ((*(p+2))<<8) + (*(p+3)))
#endif // MOTOLONG

// Must be a 32-bit target processor
#define REGISTER_WIDTH 32

#endif // __PNGENC__
