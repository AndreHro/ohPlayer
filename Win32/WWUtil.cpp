//
// Source URL:
// https://code.google.com/p/bitspersampleconv2/source/browse/#svn%2Ftrunk%2FWWMFResamplerTest
//
// Copyright (c) <year> <copyright holders>
//
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "WWUtil.h"

void
WWWaveFormatDebug(WAVEFORMATEX *v)
{
    (void)v;

    dprintf(
        "  cbSize=%d\n"
        "  nAvgBytesPerSec=%d\n"
        "  nBlockAlign=%d\n"
        "  nChannels=%d\n"
        "  nSamplesPerSec=%d\n"
        "  wBitsPerSample=%d\n"
        "  wFormatTag=0x%x\n",
        v->cbSize,
        v->nAvgBytesPerSec,
        v->nBlockAlign,
        v->nChannels,
        v->nSamplesPerSec,
        v->wBitsPerSample,
        v->wFormatTag);
}

void
WWWFEXDebug(WAVEFORMATEXTENSIBLE *v)
{
    (void)v;

    dprintf(
        "  dwChannelMask=0x%x\n"
        "  Samples.wValidBitsPerSample=%d\n"
        "  SubFormat=0x%x\n",
        v->dwChannelMask,
        v->Samples.wValidBitsPerSample,
        v->SubFormat);
}

