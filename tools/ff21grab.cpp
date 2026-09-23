/*
 * ff21grab -- dump SD PAL VBI teletext lines from a DeckLink capture card.
 *
 * A DeckLink counterpart to libajantv2's ntv2line21grab, writing the same text
 * format so tools/teletext_vbi_check.py reads either without knowing which card
 * produced it. That is the point of it: the AJA tool can only watch whatever is
 * patched to an AJA card, and comparing our teletext against another inserter's
 * means capturing both the same way.
 *
 *   tools/ff21grab --dump -n 40 -o poli.txt
 *
 * Teletext on 625-line SD sits on line 21 of field 1 and line 334 of field 2,
 * so those are the two lines dumped. Unlike the AJA tool there is no option to
 * dump the whole raster: DeckLink hands VBI lines back one at a time through
 * IDeckLinkVideoFrameAncillary, and the per-line statistics for the other 610
 * lines were read by nothing.
 *
 * Build against the SDK bundled in this tree:
 *
 *   g++ -O2 -std=c++11 -o tools/ff21grab tools/ff21grab.cpp \
 *       decklink/Blackmagic_DeckLink_SDK_15.0/Linux/include/DeckLinkAPIDispatch.cpp \
 *       -Idecklink/Blackmagic_DeckLink_SDK_15.0/Linux/include -ldl -lpthread
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>
#include <unistd.h>

#include "DeckLinkAPI.h"

using std::cerr;
using std::endl;
using std::string;

namespace {

/* Field 1 carries line 21, field 2 the line 313 lines further on. */
const uint32_t kLineField1 = 21;
const uint32_t kLineField2 = 334;
const int kSamplesPerLine = 720;

const char kHexDigits[] = "0123456789abcdef";

void AppendHexByte(string &out, uint8_t value)
{
    out += kHexDigits[value >> 4];
    out += kHexDigits[value & 0x0F];
}

/* 64 samples per output row, indented to line up under the "luma: " label,
 * matching ntv2line21grab so a diff of two captures stays readable. */
void AppendHexBlock(string &out, const uint8_t *bytes, int count, int stride)
{
    for (int i = 0; i < count; i++) {
        if (i > 0 && (i % 64) == 0)
            out += "\n        ";
        for (int b = 0; b < stride; b++)
            AppendHexByte(out, bytes[i * stride + b]);
    }
}

struct Options {
    int         frames      = 50;
    int         deviceIndex = 0;
    string      outputPath;
    bool        dump        = false;
};

class Grabber : public IDeckLinkInputCallback {
public:
    Grabber(const Options &opts, std::ostream &out)
        : mOpts(opts), mOut(out) {}

    /* IUnknown. Single-threaded ownership: the object outlives the stream. */
    HRESULT QueryInterface(REFIID, void **) override { return E_NOINTERFACE; }
    ULONG AddRef() override { return ++mRefCount; }
    ULONG Release() override { return --mRefCount; }

    HRESULT VideoInputFormatChanged(BMDVideoInputFormatChangedEvents,
                                    IDeckLinkDisplayMode *mode,
                                    BMDDetectedVideoInputFormatFlags) override
    {
        if (mode) {
            const char *name = nullptr;
            if (mode->GetName(&name) == S_OK && name) {
                cerr << "## Input changed to " << name << endl;
                free((void *)name);
            }
        }
        return S_OK;
    }

    HRESULT VideoInputFrameArrived(IDeckLinkVideoInputFrame *videoFrame,
                                   IDeckLinkAudioInputPacket *) override
    {
        if (!videoFrame || mDone)
            return S_OK;

        if (videoFrame->GetFlags() & bmdFrameHasNoInputSource) {
            mNoSignal++;
            return S_OK;
        }

        /* DeckLink reports each frame's position on the input stream, so a gap
         * between one frame and the next is exactly the frames the card could
         * not deliver -- the equivalent of AutoCirculate's dropped count, and
         * the thing that makes a capture a sampled subset without saying so. */
        BMDTimeValue frameTime = 0, frameDuration = 0;
        if (videoFrame->GetStreamTime(&frameTime, &frameDuration, kTimeScale) == S_OK
            && frameDuration > 0) {
            if (mHaveLastTime) {
                const BMDTimeValue expected = mLastTime + mLastDuration;
                if (frameTime > expected)
                    mDropped += (unsigned)((frameTime - expected) / frameDuration);
            }
            mLastTime = frameTime;
            mLastDuration = frameDuration;
            mHaveLastTime = true;
        }

        IDeckLinkVideoFrameAncillary *vanc = nullptr;
        if (videoFrame->GetAncillaryData(&vanc) != S_OK || !vanc) {
            mNoVanc++;
            return S_OK;
        }

        DumpFrame(videoFrame, vanc);
        vanc->Release();

        if (++mCaptured >= mOpts.frames)
            mDone = true;
        return S_OK;
    }

    bool done() const { return mDone; }
    unsigned captured() const { return mCaptured; }
    unsigned dropped() const { return mDropped; }
    unsigned noSignal() const { return mNoSignal; }
    unsigned noVanc() const { return mNoVanc; }

private:
    static const BMDTimeScale kTimeScale = 25;   /* frame units for 625i50 */

    void DumpLine(IDeckLinkVideoFrameAncillary *vanc, uint32_t line, int field,
                  int offset)
    {
        void *raw = nullptr;
        if (vanc->GetBufferForVerticalBlankingLine(line, &raw) != S_OK || !raw)
            return;

        /* 8-bit YUV VBI is UYVY: luma in the odd bytes, same layout the AJA
         * tool unpacks, so the luma hex of the two files is comparable. */
        const uint8_t *uyvy = (const uint8_t *)raw;
        uint8_t luma[kSamplesPerLine];
        int lo = 255, hi = 0, sum = 0;
        for (int i = 0; i < kSamplesPerLine; i++) {
            const uint8_t y = uyvy[i * 2 + 1];
            luma[i] = y;
            if (y < lo) lo = y;
            if (y > hi) hi = y;
            sum += y;
        }

        mOut << "L" << std::setw(3) << line
             << " F" << field
             << " [off=" << std::setw(3) << offset << "]"
             << "  min=" << std::setw(3) << lo
             << " max=" << std::setw(3) << hi
             << " mean=" << std::setw(3) << (sum / kSamplesPerLine)
             << " spread=" << std::setw(3) << (hi - lo);

        string block;
        block.reserve(kSamplesPerLine * 4 + 256);
        AppendHexBlock(block, luma, kSamplesPerLine, 1);
        mOut << "\n  luma: " << block;

        /* The raw UYVY adds only the chroma, which has been neutral in every
         * capture taken, and it is two thirds of the bytes. Once per file is
         * enough to show it; the checker says chroma is unchecked when a file
         * has none rather than passing silently. */
        if (!mRawWritten) {
            block.clear();
            AppendHexBlock(block, uyvy, kSamplesPerLine, 2);
            mOut << "\n  raw:  " << block;
            mRawWritten = true;
        }
        mOut << "\n";
    }

    void DumpFrame(IDeckLinkVideoInputFrame *frame,
                   IDeckLinkVideoFrameAncillary *vanc)
    {
        mOut << "=== FRAME DUMP ===\n";
        mOut << "Frame: " << mCaptured << "\n";
        mOut << "Dropped: " << mDropped << "\n";
        mOut << "Format: 625i50\n";
        mOut << "PixelFormat: bmdFormat8BitYUV\n";
        mOut << "VANCMode: DeckLinkAncillary\n";
        mOut << "TotalRasterLines: 625\n";
        mOut << "Interlaced: yes\n";
        mOut << "BufferSize: " << (frame->GetRowBytes() * frame->GetHeight()) << "\n";
        mOut << "\n";

        DumpLine(vanc, kLineField1, 1, 0);
        DumpLine(vanc, kLineField2, 2, 1);

        mOut << "=== END DUMP ===\n";
        mOut.flush();
    }

    const Options  &mOpts;
    std::ostream   &mOut;
    std::atomic<ULONG> mRefCount{1};
    unsigned        mCaptured = 0;
    unsigned        mDropped = 0;
    unsigned        mNoSignal = 0;
    unsigned        mNoVanc = 0;
    bool            mDone = false;
    bool            mRawWritten = false;
    bool            mHaveLastTime = false;
    BMDTimeValue    mLastTime = 0;
    BMDTimeValue    mLastDuration = 0;
};

void Usage(const char *argv0)
{
    cerr << "usage: " << argv0 << " --dump [-n frames] [-o file] [-d index]\n"
         << "  --dump        write the VBI dump (the only mode; accepted for\n"
         << "                symmetry with ntv2line21grab)\n"
         << "  -n frames     frames to capture (default 50)\n"
         << "  -o file       output file (default stdout)\n"
         << "  -d index      DeckLink device index (default 0)\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::ios_base::sync_with_stdio(false);

    Options opts;
    for (int i = 1; i < argc; i++) {
        const string arg(argv[i]);
        if (arg == "--dump")                      opts.dump = true;
        else if (arg == "-n" && i + 1 < argc)     opts.frames = atoi(argv[++i]);
        else if (arg == "-o" && i + 1 < argc)     opts.outputPath = argv[++i];
        else if (arg == "-d" && i + 1 < argc)     opts.deviceIndex = atoi(argv[++i]);
        else if (arg == "-h" || arg == "--help")  { Usage(argv[0]); return 0; }
        else { cerr << "## ERROR: unrecognised argument '" << arg << "'\n"; Usage(argv[0]); return 1; }
    }
    if (opts.frames < 1) {
        cerr << "## ERROR: -n must be at least 1" << endl;
        return 1;
    }

    IDeckLinkIterator *iterator = CreateDeckLinkIteratorInstance();
    if (!iterator) {
        cerr << "## ERROR: no DeckLink drivers present" << endl;
        return 1;
    }

    IDeckLink *device = nullptr;
    for (int index = 0; iterator->Next(&device) == S_OK; index++) {
        if (index == opts.deviceIndex)
            break;
        device->Release();
        device = nullptr;
    }
    iterator->Release();
    if (!device) {
        cerr << "## ERROR: no DeckLink device at index " << opts.deviceIndex << endl;
        return 1;
    }

    const char *name = nullptr;
    if (device->GetDisplayName(&name) == S_OK && name) {
        cerr << "## Device: " << name << endl;
        free((void *)name);
    }

    IDeckLinkInput *input = nullptr;
    if (device->QueryInterface(IID_IDeckLinkInput, (void **)&input) != S_OK || !input) {
        cerr << "## ERROR: device has no capture input" << endl;
        device->Release();
        return 1;
    }

    std::ofstream file;
    if (!opts.outputPath.empty()) {
        file.open(opts.outputPath.c_str(), std::ios::binary);
        if (!file.good()) {
            cerr << "## ERROR: cannot open '" << opts.outputPath << "'" << endl;
            input->Release(); device->Release();
            return 1;
        }
    }
    std::ostream &out = opts.outputPath.empty() ? std::cout : file;

    Grabber grabber(opts, out);
    input->SetCallback(&grabber);

    /* 8-bit YUV so the VBI lines come back as UYVY, which is the layout the AJA
     * tool unpacks and what libavdevice's own PAL teletext path expects. */
    if (input->EnableVideoInput(bmdModePAL, bmdFormat8BitYUV, bmdVideoInputFlagDefault) != S_OK) {
        cerr << "## ERROR: cannot enable 625i50 8-bit input" << endl;
        input->SetCallback(nullptr); input->Release(); device->Release();
        return 1;
    }

    cerr << "## PAL 625i -- capturing lines " << kLineField1 << " and "
         << kLineField2 << " for " << opts.frames << " frames" << endl;

    if (input->StartStreams() != S_OK) {
        cerr << "## ERROR: cannot start capture" << endl;
        input->DisableVideoInput();
        input->SetCallback(nullptr); input->Release(); device->Release();
        return 1;
    }

    while (!grabber.done())
        usleep(20000);

    input->StopStreams();
    input->DisableVideoInput();
    input->SetCallback(nullptr);

    cerr << "## Capture done: " << grabber.captured() << " frames transferred, "
         << grabber.dropped() << " dropped" << endl;
    if (grabber.noSignal())
        cerr << "## WARNING: " << grabber.noSignal()
             << " frames arrived with no input source" << endl;
    if (grabber.noVanc())
        cerr << "## WARNING: " << grabber.noVanc()
             << " frames carried no ancillary data -- this card or mode may not"
                " expose SD VBI on input" << endl;
    if (grabber.dropped())
        cerr << "## WARNING: " << grabber.dropped() << " frames were dropped, so this"
                " capture is a sampled subset of the output -- roughly 1 frame in "
             << ((grabber.dropped() + grabber.captured()) / grabber.captured())
             << ". Frame numbering stays consecutive regardless, so do not read"
                " anything from it about timing between frames." << endl;

    input->Release();
    device->Release();
    return 0;
}
