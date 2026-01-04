//
// PNG Decoder
//
// written by Larry Bank
// bitbank@pobox.com
// Arduino port started 5/3/2021
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
#include "PNGenc.h"

// Include the C code which does the actual work
//
// Embedded-friendly PNG Encoder
//
// Copyright (c) 2000-2021 BitBank Software, Inc.
// Written by Larry Bank
// Project started 12/9/2000
//
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"
#include "zutil.h"
#include "deflate.h"
#include "PNGenc.h"

//
// our private structure to hold a JPEG image decode state
//
typedef struct PNGENCIMAGE
{
    int iWidth, iHeight, y, iTransparent; // image size
    uint8_t ucBpp, ucPixelType, ucCompLevel, ucHasAlphaPalette;
    uint8_t ucMemType;
    uint8_t *pOutput;
    int iBufferSize; // output buffer size provided by caller
    int iHeaderSize; // size of the PNG header
    int iCompressedSize; // size of flate output
    int iDataSize; // total output file size
    int iMemPool; // memory allocated out of memory pool
    int iError;
    PNGFILE PNGFile;
    // Optional file callback pointers (may be NULL). Present to keep
    // legacy file-based code compilable, but the public C++ API uses
    // buffer-based output only (`open(uint8_t*, int)`).
    PNGENC_SEEK_CALLBACK *pfnSeek;
    z_stream c_stream; /* compression stream */
    uint8_t ucPalette[1024];
    uint8_t ucMemPool[sizeof(deflate_state) + (0x40000 >> MEM_SHRINK)]; // RAM needed for deflate
    uint8_t ucPrevLine[PNG_MAX_BUFFERED_PIXELS];
    uint8_t ucCurrLine[PNG_MAX_BUFFERED_PIXELS];
    uint8_t ucFileBuf[PNG_FILE_BUF_SIZE]; // holds temp file data
} PNGENCIMAGE;

PNGenc::PNGenc() :
    _png(new PNGENCIMAGE)
{}
PNGenc::~PNGenc()
{
    delete _png; 
}    
// Macro to simplify writing a big-endian 32-bit value on any CPU
#define WRITEMOTO32(p, o, val) {uint32_t l = val; p[o] = (unsigned char)(l >> 24); p[o+1] = (unsigned char)(l >> 16); p[o+2] = (unsigned char)(l >> 8); p[o+3] = (unsigned char)l;}

unsigned char PNGFindFilter(uint8_t *pCurr, uint8_t *pPrev, int iPitch, int iStride);
void PNGFilter(uint8_t ucFilter, uint8_t *pOut, uint8_t *pCurr, uint8_t *pPrev, int iStride, int iPitch);
//
// Calculate the PNG-style CRC value for a block of data
//
uint32_t PNGCalcCRC(unsigned char *buf, int len, uint32_t u32_start)
{
/* Table of CRCs of all 8-bit messages. */
   static uint32_t crc_table[256];
   static int crc_table_computed = 0;
    uint32_t crc = u32_start; //0xffffffff;
   int n;

   /* Make the table for a fast CRC. */
   if (crc_table_computed == 0)
   {
     uint32_t c;
     int n, k;
     
     for (n = 0; n < 256; n++) {
       c = (uint32_t) n;
       for (k = 0; k < 8; k++) {
         if (c & 1)
           c = 0xedb88320L ^ (c >> 1);
         else
           c = c >> 1;
       }
       crc_table[n] = c;
     }
     crc_table_computed = 1;
   }

   /* Update a running CRC with the bytes buf[0..len-1]--the CRC
      should be initialized to all 1's, and the transmitted value
      is the 1's complement of the final running CRC (see the
      crc() routine below)). */

     for (n = 0; n < len; n++)
     {
         crc = crc_table[(crc ^ buf[n]) & 0xff] ^ (crc >> 8);
     }
return crc;  //return crc ^ 0xffffffffL precludes chaining the calculation of packets for IDAT chunck;

} /* PNGCalcCRC() */

static unsigned char PAETH(unsigned char a, unsigned char b, unsigned char c)
{
    int pa, pb, pc;
#ifdef SLOW_WAY
    int p;
#endif // SLOW_WAY
    
#ifndef SLOW_WAY
    pc = c;
    pa = b - pc;
    pb = a - pc;
    pc = pa + pb;
    if (pa < 0) pa = -pa;
    if (pb < 0) pb = -pb;
    if (pc < 0) pc = -pc;
#else
    p = a + b - c; // initial estimate
    pa = abs(p - a); // distances to a, b, c
    pb = abs(p - b);
    pc = abs(p - c);
#endif
    // return nearest of a,b,c,
    // breaking ties in order a,b,c.
    if (pa <= pb && pa <= pc)
        return a;
    else if (pb <= pc)
        return b;
    else return c;
    
} /* PAETH() */
//
// Write the PNG file header and, if needed, a color palette chunk
//
static int PNGStartFile(PNGENCIMAGE *pImage)
{
    int iError = PNG_SUCCESS;
    unsigned char *p;
    int iSize, i, iLen;
    uint32_t ulCRC;
        
    p = pImage->ucFileBuf;
    iSize = 0; // output data size
    WRITEMOTO32(p, iSize, 0x89504e47); // PNG File header
    iSize += 4;
    WRITEMOTO32(p, iSize, 0x0d0a1a0a);
    iSize += 4;
    // IHDR contains 13 data bytes
    WRITEMOTO32(p, iSize, 0x0000000d); // IHDR length
    iSize += 4;
    WRITEMOTO32(p, iSize, 0x49484452); // IHDR marker
    iSize += 4;
    WRITEMOTO32(p, iSize, pImage->iWidth); // Image Width
    iSize += 4;
    WRITEMOTO32(p, iSize, pImage->iHeight); // Image Height
    iSize += 4;
    p[iSize++] = (pImage->ucBpp > 8) ? 8:pImage->ucBpp; // Bit depth
    p[iSize++] = pImage->ucPixelType;
    p[iSize++] = 0; // compression method 0
    p[iSize++] = 0; // filter type 0
    p[iSize++] = 0; // interlace = no
    ulCRC = PNGCalcCRC(&p[iSize-17], 17, 0xffffffff); // store CRC for IHDR chunk
    ulCRC=ulCRC ^ 0xffffffffL;// terminate CRC for IHDR chunk
	WRITEMOTO32(p, iSize, ulCRC);
    iSize += 4;

    if (pImage->ucPixelType == PNG_PIXEL_INDEXED)
	   {
           // Write the palette
           iLen = (1 << pImage->ucBpp); // palette length
           WRITEMOTO32(p, iSize, iLen*3); // 3 bytes per entry
           iSize += 4;
           WRITEMOTO32(p, iSize, 0x504c5445/*'PLTE'*/);
           iSize += 4;
           for (i=0; i<iLen; i++)
           {
               p[iSize++] = pImage->ucPalette[i*3+2]; // red
               p[iSize++] = pImage->ucPalette[i*3+1]; // green
               p[iSize++] = pImage->ucPalette[i*3+0]; // blue
           }
           ulCRC = PNGCalcCRC(&p[iSize-(iLen*3)-4], 4+(iLen*3), 0xffffffff); // store CRC for PLTE chunk
           ulCRC=ulCRC ^ 0xffffffffL;// terminate CRC for PLTE chunk
	WRITEMOTO32(p, iSize, ulCRC);
           iSize += 4;
           if (pImage->iTransparent >= 0 || pImage->ucHasAlphaPalette) // add transparency chunk
           {
               if (pImage->ucPixelType == PNG_PIXEL_INDEXED) { // a set of palette alpha values
                    iLen = (1 << pImage->ucBpp); // palette length
               } else if (pImage->ucPixelType == PNG_PIXEL_GRAYSCALE) {
                   iLen = 2;
               } else {
                   iLen = 6; // truecolor single transparent color
               }
               WRITEMOTO32(p, iSize, iLen); // 1 byte per palette alpha entry
               iSize += 4;
               WRITEMOTO32(p, iSize, 0x74524e53 /*'tRNS'*/);
               iSize += 4;
               switch (iLen) {
                   case 2: // grayscale
                       p[iSize++] = 0; // 16-bit value (big endian)
                       p[iSize++] = (uint8_t)pImage->iTransparent;
                       break;
                   case 6: // truecolor
                       p[iSize++] = 0; // 16-bit value (big endian for color stimulus)
                       p[iSize++] = (uint8_t)pImage->iTransparent & 0xff;
                       p[iSize++] = 0;
                       p[iSize++] = (uint8_t)((pImage->iTransparent >> 8) & 0xff);
                       p[iSize++] = 0;
                       p[iSize++] = (uint8_t)((pImage->iTransparent >> 16) & 0xff);
                       p[iSize++] = 0;
                       break;
                   default: // palette colors
                       for (i = 0; i<iLen; i++) // write n alpha values to accompany the palette
                       {
                           p[iSize++] = pImage->ucPalette[768+i];
                       }
                       break;
               } // switch
               ulCRC = PNGCalcCRC(&p[iSize - iLen - 4], 4 + iLen, 0xffffffff); // store CRC for tRNS chunk
               ulCRC=ulCRC ^ 0xffffffffL;// terminate CRC for tRNS chunk
		WRITEMOTO32(p, iSize, ulCRC);
               iSize += 4;
           }
       }
    // IDAT
    WRITEMOTO32(p, iSize, 0/*iCompressedSize*/); // IDAT length
    iSize += 4;
    WRITEMOTO32(p, iSize, 0x49444154); // IDAT marker
    iSize += 4;
    pImage->iCompressedSize = 0;
    pImage->iHeaderSize = iSize; // keep the PNG header size for later
    if (pImage->pOutput) { // copy to ram?
        memcpy(pImage->pOutput, pImage->ucFileBuf, iSize);
    }
    return iError;
    
} /* PNGStartFile() */
//
// Finish PNG file data (updates IDAT chunk size+crc & writes END chunk)
//
int PNGEndFile(PNGENCIMAGE *pImage)
{
    int iSize=0;
    uint8_t *p;
    uint32_t ulCRC;
    
    if (pImage->pOutput) { // output buffer = easy to wrap up
        p = pImage->pOutput;
        iSize = pImage->iHeaderSize;
        WRITEMOTO32(p, iSize-8, pImage->iCompressedSize); // write IDAT chunk size
        iSize += pImage->iCompressedSize;
        ulCRC = PNGCalcCRC(&p[iSize-pImage->iCompressedSize-4], pImage->iCompressedSize+4, 0xffffffff); // store CRC for IDAT chunk
        ulCRC=ulCRC ^ 0xffffffffL;// terminate CRC for IDAT chunk
	    WRITEMOTO32(p, iSize, ulCRC);
        iSize += 4;
        // Write the IEND chunk
        WRITEMOTO32(p, iSize, 0);
        iSize += 4;
        WRITEMOTO32(p, iSize, 0x49454e44/*'IEND'*/);
        iSize += 4;
        WRITEMOTO32(p, iSize, 0xae426082); // same CRC every time
        iSize += 4;
    }
    return iSize;
} /* PNGEndFile() */

//
// My internal alloc/free functions to work on simple embedded systems
//
voidpf ZLIB_INTERNAL myalloc (voidpf opaque, unsigned int items, unsigned int size)
{
    PNGENCIMAGE *pImage = (PNGENCIMAGE *)opaque;
    // allocate from our internal pool
    int iSize = items * size;
    void *p = &pImage->ucMemPool[pImage->iMemPool];
    pImage->iMemPool += iSize;
    return p;
} /* myalloc() */

void ZLIB_INTERNAL myfree (voidpf opaque, voidpf ptr)
{
    (void)opaque;
    (void)ptr; // doesn't do anything since the memory is from an internal pool
} /* myfree() */
//
// Compress one line of image at a time and write the compressed data
// incrementally to the output file. This allows the system to not need an
// input nor output buffer larger than 2 lines of image data
//
int PNG_encodeBegin(PNGENCIMAGE *pPNG, int iWidth, int iHeight, uint8_t ucPixelType, uint8_t ucBpp, uint8_t *pPalette, uint8_t ucCompLevel)
{
    pPNG->iWidth = iWidth;
    pPNG->iHeight = iHeight;
    pPNG->ucPixelType = ucPixelType;
    pPNG->ucBpp = ucBpp;
    if (pPalette != NULL)
        memcpy(pPNG->ucPalette, pPalette, 768); // save 256 color entries
    pPNG->ucCompLevel = ucCompLevel;
    pPNG->y = 0;
    return PNG_SUCCESS;
} /* PNG_encodeBegin() */

int PNG_addLine(PNGENCIMAGE *pImage, uint8_t *pSrc, int y)
{
    unsigned char ucFilter; // filter type
    unsigned char *pOut;
    int iStride;
    int err;
    int iPitch;
    
    iStride = pImage->ucBpp >> 3; // bytes per pixel
    if (pImage->ucBpp == 1) {
        iPitch = (pImage->iWidth + 7)/8;
    } else if (pImage->ucBpp == 2) {
        iPitch = (pImage->iWidth + 3)/4;
    } else if (pImage->ucBpp == 4) {
        iPitch = (pImage->iWidth + 1)/2;
    } else {
        iPitch = (pImage->iWidth * pImage->ucBpp) >> 3;
    }
    pOut = pImage->ucCurrLine;
    if (iStride < 1) {
        iStride = 1; // 1,4 bpp
        ucFilter = 0; // filtering doesn't seem to improve low color images
    } else {
        ucFilter = PNGFindFilter(pSrc, (y == 0) ? NULL : pImage->ucPrevLine, iPitch, iStride); // find best filter
    }
    *pOut++ = ucFilter; // store filter byte
    PNGFilter(ucFilter, pOut, pSrc, pImage->ucPrevLine, iStride, iPitch); // filter the current line of image data and store
    memcpy(pImage->ucPrevLine, pSrc, iPitch);
    // Compress the filtered image data
    if (y == 0) // first block, initialize zlib
    {
        PNGStartFile(pImage);
        memset(&pImage->c_stream, 0, sizeof(z_stream));
        pImage->c_stream.zalloc = myalloc; // use internal alloc/free
        pImage->c_stream.zfree = myfree; // to use our memory pool
        pImage->c_stream.opaque = (voidpf)pImage;
        pImage->iMemPool = 0;
        // ZLIB compression levels: 1 = fastest, 9 = most compressed (slowest)
//        err = deflateInit(&pImage->c_stream, pImage->ucCompLevel); // might as well use max compression
        err = deflateInit2_(&pImage->c_stream, pImage->ucCompLevel, Z_DEFLATED, MAX_WBITS-MEM_SHRINK, DEF_MEM_LEVEL-MEM_SHRINK, Z_DEFAULT_STRATEGY, ZLIB_VERSION, (int)sizeof(z_stream)); // might as well use max compression
        pImage->c_stream.total_out = 0;
        pImage->c_stream.total_in = 0;
        pImage->c_stream.next_out = pImage->ucFileBuf;
        pImage->c_stream.avail_out = PNG_FILE_BUF_SIZE;
    }
    pImage->c_stream.next_in  = (Bytef*)pImage->ucCurrLine;
    pImage->c_stream.avail_in = iPitch+1; // compress entire buffer in 1 shot
    err = deflate(&pImage->c_stream, Z_NO_FLUSH); // Z_FULL_FLUSH);
    if (err != Z_OK) { // something went wrong with the data compression, stop
        pImage->iError = PNG_ENCODE_ERROR;
        return PNG_ENCODE_ERROR;
    }
    if (y == pImage->iHeight - 1) // last line, clean up
    {
        err = deflate(&pImage->c_stream, Z_FULL_FLUSH);
        err = deflate(&pImage->c_stream, Z_FINISH); // flush any remaining output
        while(err == Z_OK || err == Z_BUF_ERROR) { // more data than will fit
            if (pImage->pOutput) { // memory
                if ((pImage->iHeaderSize + pImage->iCompressedSize + pImage->c_stream.total_out) > pImage->iBufferSize) {
                    // output buffer not large enough
                    pImage->iError = PNG_MEM_ERROR;
                    return PNG_MEM_ERROR;
                }
                memcpy(&pImage->pOutput[pImage->iHeaderSize + pImage->iCompressedSize], pImage->ucFileBuf, pImage->c_stream.total_out);
            }
            pImage->iCompressedSize += (int)pImage->c_stream.total_out;
            // reset zlib output buffer to start
            pImage->c_stream.total_out = 0;
            pImage->c_stream.next_out = pImage->ucFileBuf;
            pImage->c_stream.avail_out = PNG_FILE_BUF_SIZE;
            err = deflate(&pImage->c_stream, Z_FINISH);
        }
        err = deflateEnd(&pImage->c_stream);
        if (pImage->c_stream.total_out) { // the last bytes of the compressed data stream
            if (pImage->pOutput) { // memory
                if ((pImage->iHeaderSize + pImage->iCompressedSize + pImage->c_stream.total_out) > pImage->iBufferSize) {
                    // output buffer not large enough
                    pImage->iError = PNG_MEM_ERROR;
                    return PNG_MEM_ERROR;
                }
                memcpy(&pImage->pOutput[pImage->iHeaderSize + pImage->iCompressedSize], pImage->ucFileBuf, pImage->c_stream.total_out);
            }
            pImage->iCompressedSize += (int)pImage->c_stream.total_out;
            pImage->c_stream.total_out = 0;
        }
    }
    // Write the data to memory or a file
    //
    // A bunch of extra logic has been added below to minimize the total number
    // of calls to 'write'. Each compressed scanline might generate only a few
    // bytes of flate output and calling write() for a few bytes at a time can
    // slow things to a crawl.
    while (pImage->c_stream.total_out >= PNG_FILE_HIGHWATER || pImage->c_stream.avail_in != 0) {
        if (pImage->pOutput) { // memory
            if ((pImage->iHeaderSize + pImage->iCompressedSize + pImage->c_stream.total_out) > pImage->iBufferSize) {
                // output buffer not large enough
                pImage->iError = PNG_MEM_ERROR;
                return PNG_MEM_ERROR;
            }
            memcpy(&pImage->pOutput[pImage->iHeaderSize + pImage->iCompressedSize], pImage->ucFileBuf, pImage->c_stream.total_out);
        }
        pImage->iCompressedSize += (int)pImage->c_stream.total_out;
        // reset zlib output buffer to start
        pImage->c_stream.total_out = 0;
        pImage->c_stream.next_out = pImage->ucFileBuf;
        pImage->c_stream.avail_out = PNG_FILE_BUF_SIZE;
        if (pImage->c_stream.avail_in != 0) { // left over data that it didn't have room to compress
            err = deflate(&pImage->c_stream, Z_NO_FLUSH);
        }
    } // highwater hit
    if (y == pImage->iHeight -1) { // last line, finish file
        pImage->iDataSize = PNGEndFile(pImage);
    }    
    return PNG_SUCCESS; // DEBUG
    
} /* PNG_addLine() */
//
// Compress one line image at a time and write the compressed data
// incrementally to the output file. This allows the system to not need an
// input nor output buffer larger than 2 lines of image data
// The input pixels are RGB565 (not supported by PNG) and are converted into the
// format requested by the iPixelType param in the call to encodeBegin()
//
int PNG_addRGB565Line(PNGENCIMAGE *pImage, uint16_t *pRGB565, void *pTempLine, int y, int bBigEndian)
{
    unsigned char ucFilter; // filter type
    unsigned char *pOut, *pSrc;
    int iStride;
    int err;
    int iPitch;
    uint16_t us, *s = pRGB565;
    uint8_t *d = (uint8_t *)pTempLine;

    switch (pImage->ucPixelType) {
        case PNG_PIXEL_TRUECOLOR:
            for (int i=0; i<pImage->iWidth; i++) {
                us = *s++;
                if (bBigEndian) {
                    us = __builtin_bswap16(us);
                }
                *d++ = (uint8_t)(((us >> 8) & 0xf8) | (us >> 13)); // red
                *d++ = (uint8_t)(((us >> 3) & 0xfc) | ((us >> 9) & 0x3)); // green
                *d++ = (uint8_t)(((us & 0x1f) << 3) | ((us & 0x1c) >> 2)); // blue
            }
            break;
        case PNG_PIXEL_GRAYSCALE:
            for (int i=0; i<pImage->iWidth; i++) {
                int r, g, b;
                us = *s++;
                if (bBigEndian) {
                    us = __builtin_bswap16(us);
                }
                r = (uint8_t)(((us >> 8) & 0xf8) | (us >> 13)); // red
                g = (uint8_t)(((us >> 3) & 0xfc) | ((us >> 9) & 0x3)); // green
                b = (uint8_t)(((us & 0x1f) << 3) | ((us & 0x1c) >> 2)); // blue
                *d++ = (uint8_t)((r + g*2 + b)>>2);
            }
            break;
        // Note - other pixel types don't make sense to support coming from RGB565
        default: // not a valid pixel type
            pImage->iError = PNG_INVALID_PARAMETER;
            return PNG_INVALID_PARAMETER;
    }
    pSrc = (uint8_t *)pTempLine;
    iStride = pImage->ucBpp >> 3; // bytes per pixel
    iPitch = (pImage->iWidth * pImage->ucBpp) >> 3;
    if (iStride < 1)
        iStride = 1; // 1,4 bpp
    pOut = pImage->ucCurrLine;
    ucFilter = PNGFindFilter(pSrc, (y == 0) ? NULL : pImage->ucPrevLine, iPitch, iStride); // find best filter
    *pOut++ = ucFilter; // store filter byte
    PNGFilter(ucFilter, pOut, pSrc, pImage->ucPrevLine, iStride, iPitch); // filter the current line of image data and store
    memcpy(pImage->ucPrevLine, pSrc, iPitch);
    // Compress the filtered image data
    if (y == 0) // first block, initialize zlib
    {
        PNGStartFile(pImage);
        memset(&pImage->c_stream, 0, sizeof(z_stream));
        pImage->c_stream.zalloc = myalloc; // use internal alloc/free
        pImage->c_stream.zfree = myfree; // to use our memory pool
        pImage->c_stream.opaque = (voidpf)pImage;
        pImage->iMemPool = 0;
        // ZLIB compression levels: 1 = fastest, 9 = most compressed (slowest)
//        err = deflateInit(&pImage->c_stream, pImage->ucCompLevel); // might as well use max compression
        err = deflateInit2_(&pImage->c_stream, pImage->ucCompLevel, Z_DEFLATED, MAX_WBITS-MEM_SHRINK, DEF_MEM_LEVEL-MEM_SHRINK, Z_DEFAULT_STRATEGY, ZLIB_VERSION, (int)sizeof(z_stream)); // might as well use max compression
        pImage->c_stream.total_out = 0;
        pImage->c_stream.next_out = pImage->ucFileBuf;
        pImage->c_stream.avail_out = PNG_FILE_BUF_SIZE;
    }
    pImage->c_stream.next_in  = (Bytef*)pImage->ucCurrLine;
    pImage->c_stream.total_in = 0;
    pImage->c_stream.avail_in = iPitch+1; // compress entire buffer in 1 shot
    err = deflate(&pImage->c_stream, Z_PARTIAL_FLUSH); // Z_SYNC_FLUSH);
    if (err != Z_OK) { // something went wrong with the data compression, stop
        pImage->iError = PNG_ENCODE_ERROR;
        return PNG_ENCODE_ERROR;
    }
    if (y == pImage->iHeight - 1) // last line, clean up
    {
        err = deflate(&pImage->c_stream, Z_FINISH);
        err = deflateEnd(&pImage->c_stream);
    }
    // Write the data to memory or a file
    //
    // A bunch of extra logic has been added below to minimize the total number
    // of calls to 'write'. Each compressed scanline might generate only a few
    // bytes of flate output and calling write() for a few bytes at a time can
    // slow things to a crawl.
    if (pImage->c_stream.total_out >= PNG_FILE_HIGHWATER) {
        if (pImage->pOutput) { // memory
            if ((pImage->iHeaderSize + pImage->iCompressedSize + pImage->c_stream.total_out) > pImage->iBufferSize) {
                // output buffer not large enough
                pImage->iError = PNG_MEM_ERROR;
                return PNG_MEM_ERROR;
            }
            memcpy(&pImage->pOutput[pImage->iHeaderSize + pImage->iCompressedSize], pImage->ucFileBuf, pImage->c_stream.total_out);
        }
        pImage->iCompressedSize += (int)pImage->c_stream.total_out;
        // reset zlib output buffer to start
        pImage->c_stream.total_out = 0;
        pImage->c_stream.next_out = pImage->ucFileBuf;
        pImage->c_stream.avail_out = PNG_FILE_BUF_SIZE;
    } // highwater hit
    if (y == pImage->iHeight -1) { // last line, finish file
        // if any remaining data in output buffer, write it
        if (pImage->c_stream.total_out > 0) {
            if (pImage->pOutput) { // memory
                if ((pImage->iHeaderSize + pImage->iCompressedSize + pImage->c_stream.total_out) > pImage->iBufferSize) {
                    // output buffer not large enough
                    pImage->iError = PNG_MEM_ERROR;
                    return PNG_MEM_ERROR;
                }
                memcpy(&pImage->pOutput[pImage->iHeaderSize + pImage->iCompressedSize], pImage->ucFileBuf, pImage->c_stream.total_out);
            }
            pImage->iCompressedSize += (int)pImage->c_stream.total_out;
        }
        pImage->iDataSize = PNGEndFile(pImage);
    }
    return PNG_SUCCESS; // DEBUG

} /* PNG_addRGB565Line() */

//
// Find the best filter method for the given scanline
// Try each filter algorithm in turn and use SAD (sum of absolute differences)
// to choose the one with the lowest sum (a reasonable proxy for entropy)
//
unsigned char PNGFindFilter(uint8_t *pCurr, uint8_t *pPrev, int iPitch, int iStride)
{
int i;
unsigned char a, b, c, ucDiff, ucFilter;
uint32_t ulSum[5]  = {0,0,0,0,0}; // individual sums for the 4 types of filters
uint32_t ulMin;

    ucFilter = 0;
    for (i=0; i<iPitch; i++)
    {
       ucDiff = pCurr[i]; // no filter
        ulSum[0] += (ucDiff < 128) ? ucDiff: 256 - ucDiff;
       // Sub
       if (i >= iStride)
       {
          ucDiff = pCurr[i]-pCurr[i-iStride];
          ulSum[1] += (ucDiff < 128) ? ucDiff: 256 - ucDiff;
       }
       else
       {
           ucDiff = pCurr[i];
           ulSum[1] += (ucDiff < 128) ? ucDiff: 256 - ucDiff;
       }
       // Up
       if (pPrev)
       {
          ucDiff = pCurr[i]-pPrev[i];
          ulSum[2] += (ucDiff < 128) ? ucDiff: 256 - ucDiff;
       }
       else // not available
       {
           ucDiff = pCurr[i];
           ulSum[2] += (ucDiff < 128) ? ucDiff: 256 - ucDiff;
       }
       // Average
       if (!pPrev || i < iStride)
       {
          if (!pPrev)
          {
             if (i < iStride)
                a = 0;
             else
                a = pCurr[i-iStride]>>1;
          }
          else
             a = pPrev[i]>>1;
       }
       else
       {
          a = (pCurr[i-iStride] + pPrev[i])>>1;
       }
       ucDiff = pCurr[i] - a;
       ulSum[3] += (ucDiff < 128) ? ucDiff: 256 - ucDiff;
       // Paeth
       if (i < iStride)
          a = 0;
       else
          a = pCurr[i-iStride]; // left
       if (pPrev == NULL)
          b = 0; // above doesn't exist
       else
          b = pPrev[i];
       if (!pPrev || i < iStride)
          c = 0;
       else
          c = pPrev[i-iStride]; // above left
       ucDiff = pCurr[i] - PAETH(a,b,c);
       ulSum[4] += (ucDiff < 128) ? ucDiff: 256 - ucDiff;
       } // for i
       // Pick the best filter (or NONE if they're all bad)
       ulMin = iPitch * 255; // max value
       for (a=0; a<5; a++)
       {
          if (ulSum[a] < ulMin)
          {
             ulMin = ulSum[a];
             ucFilter = a; // current winner
          }
       } // for
       return ucFilter;

} /* PNGFindFilter() */
//
// Apply the given filter algorithm to a line of image data
//
void PNGFilter(uint8_t ucFilter, uint8_t *pOut, uint8_t *pCurr, uint8_t *pPrev, int iStride, int iPitch)
{
int j;

   switch (ucFilter)
      {
      case 0: // no filter, just copy
         memcpy(pOut, pCurr, iPitch);
         break;
      case 1: // sub
         j = 0;
         while (j < iStride)
         {
             pOut[j] = pCurr[j];
             j++;
         }
         while (j < (int)iPitch)
         {
            pOut[j] = pCurr[j]-pCurr[j-iStride];
            j++;
         }
         break;
      case 2: // up
         if (pPrev)
         {
            for (j=0;j<iPitch;j++)
            {
               pOut[j] = pCurr[j]-pPrev[j];
            }
         }
         else
            memcpy(pOut, pCurr, iPitch);
         break;
      case 3: // average
         for (j=0; j<iPitch; j++)
        {
            int a;
            if (!pPrev || j < iStride)
            {
               if (!pPrev)
               {
                  if (j < iStride)
                     a = 0;
                  else
                     a = pCurr[j-iStride]>>1;
               }
               else
                  a = pPrev[j]>>1;
            }
            else
            {
               a = (pCurr[j-iStride] + pPrev[j])>>1;
            }
            pOut[j] = (uint8_t)(pCurr[j] - a);
         }
         break;
      case 4: // paeth
         for (j=0; j<iPitch; j++)
         {
            uint8_t a,b,c;
            if (j < iStride)
               a = 0;
            else
               a = pCurr[j-iStride]; // left
            if (!pPrev)
               b = 0; // above doesn't exist
            else
               b = pPrev[j]; // above
            if (!pPrev || j < iStride)
               c = 0;
            else
               c = pPrev[j-iStride]; // above left
            pOut[j] = pCurr[j] - PAETH(a,b,c);
         }
         break;
      } // switch
} /* PNGFilter() */

int PNG_openRAM(PNGENCIMAGE *pPNG, uint8_t *pData, int iDataSize)
{
    memset(pPNG, 0, sizeof(PNGENCIMAGE));
    pPNG->iTransparent = -1;
    pPNG->pOutput = pData;
    pPNG->iBufferSize = iDataSize;
    return PNG_SUCCESS;
} /* PNG_openRAM() */



int PNG_close(PNGENCIMAGE *pPNG)
{
    return pPNG->iDataSize;
} /* PNG_close() */


// File-based open removed: header declares buffer-only `open(uint8_t*, int)`

int PNGenc::open(uint8_t *pOutput, int iBufferSize)
{
    if (!pOutput || iBufferSize < 32) return PNG_INVALID_PARAMETER; // must have a valid buffer and minimum size
    memset(_png, 0, sizeof(PNGENCIMAGE));
    _png->iTransparent = -1;
    _png->pOutput = pOutput;
    _png->iBufferSize = iBufferSize;
    return PNG_SUCCESS;
} /* open() */

//
// return the last error (if any)
//
int PNGenc::getLastError()
{
    return _png->iError;
} /* getLastError() */
//
// Close the file - not needed when decoding from memory
//
int PNGenc::close()
{
    // Buffer-based close: nothing to do for in-memory output
    return _png->iDataSize;
} /* close() */

int PNGenc::encodeBegin(int iWidth, int iHeight, uint8_t ucPixelType, uint8_t ucBpp, uint8_t *pPalette, uint8_t ucCompLevel)
{
    // Check for valid parameters
    if (iWidth < 1 || iWidth > 32767 || iHeight < 1 || iHeight > 32767) return PNG_INVALID_PARAMETER;
    if (ucPixelType != PNG_PIXEL_GRAYSCALE && ucPixelType != PNG_PIXEL_TRUECOLOR && ucPixelType != PNG_PIXEL_INDEXED &&
        ucPixelType != PNG_PIXEL_GRAY_ALPHA && ucPixelType != PNG_PIXEL_TRUECOLOR_ALPHA) return PNG_INVALID_PARAMETER;
    if (ucBpp != 1 && ucBpp != 2 && ucBpp != 4 && ucBpp != 8 && ucBpp != 24 && ucBpp != 32) return PNG_INVALID_PARAMETER;
    if (ucCompLevel > 9) return PNG_INVALID_PARAMETER;
    
    _png->iWidth = iWidth;
    _png->iHeight = iHeight;
    _png->ucPixelType = ucPixelType;
    _png->ucBpp = ucBpp;
    if (pPalette != NULL)
        memcpy(_png->ucPalette, pPalette, 768); // save 256 color entries
    _png->ucCompLevel = ucCompLevel;
    _png->y = 0;
    return PNG_SUCCESS;
} /* encodeBegin() */

int PNGenc::addLine(uint8_t *pPixels)
{
    int rc;
    if (_png->pOutput && _png->iBufferSize) {
        rc = PNG_addLine(_png, pPixels, _png->y);
        _png->y++;
        return rc;
    } else { // the encoder was never initialized
        return PNG_NOT_INITIALIZED;
    }
} /* addLine() */

int PNGenc::addRGB565Line(uint16_t *pPixels, void *pTempLine, bool bBigEndian)
{
    int rc;
    rc = PNG_addRGB565Line(_png, pPixels, pTempLine, _png->y, (int)bBigEndian);
    _png->y++;
    return rc;
} /* addRGB565Line() */

int PNGenc::setTransparentColor(uint32_t u32Color)
{
    if (_png->ucPixelType == PNG_PIXEL_GRAYSCALE || _png->ucPixelType == PNG_PIXEL_TRUECOLOR) {
        _png->iTransparent = u32Color;
        return PNG_SUCCESS;
    }
    else
        return PNG_INVALID_PARAMETER; // indexed image must have palette alpha values
} /* setTransparentColor() */

int PNGenc::setAlphaPalette(uint8_t *pPalette)
{
    if (pPalette != NULL && _png->ucPixelType == PNG_PIXEL_INDEXED) {
        _png->ucHasAlphaPalette = 1;
        memcpy(&_png->ucPalette[768], (void*)pPalette, 256); // capture up to 256 alpha values
        return PNG_SUCCESS;
    }
    return PNG_INVALID_PARAMETER;
} /* setAlphaPalette() */
