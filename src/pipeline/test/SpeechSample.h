// Loads real speech for tests that need it (e.g. noise suppression, which
// treats synthetic "speech" as noise): rade_src/wav/all.wav from the RADE
// sources the build fetches, 16-bit mono at 16 kHz. Looks at the path the
// build passes in, then next to the test (as on the Windows CI runner).

#ifndef SPEECH_SAMPLE_H
#define SPEECH_SAMPLE_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Returns up to `seconds` of speech at 16 kHz, or nothing if the file can't
// be found or read.
inline std::vector<short> loadSpeech16k(double seconds)
{
    std::vector<std::string> paths;
#if defined(SPEECH_WAV_PATH)
    paths.push_back(SPEECH_WAV_PATH);
#endif // defined(SPEECH_WAV_PATH)
    paths.push_back("all.wav");

    for (auto& path : paths)
    {
        FILE* fp = fopen(path.c_str(), "rb");
        if (fp == nullptr)
        {
            continue;
        }

        // Walk the RIFF chunks to "data".
        char riff[12];
        std::vector<short> samples;
        if (fread(riff, 1, 12, fp) == 12 && memcmp(riff, "RIFF", 4) == 0 && memcmp(riff + 8, "WAVE", 4) == 0)
        {
            char id[4];
            uint32_t size;
            while (fread(id, 1, 4, fp) == 4 && fread(&size, 4, 1, fp) == 1)
            {
                if (memcmp(id, "data", 4) == 0)
                {
                    size_t count = std::min<size_t>(size / 2, (size_t)(seconds * 16000));
                    samples.resize(count);
                    samples.resize(fread(samples.data(), 2, count, fp));
                    break;
                }
                fseek(fp, size + (size & 1), SEEK_CUR);
            }
        }
        fclose(fp);
        if (!samples.empty())
        {
            return samples;
        }
    }
    return {};
}

#endif // SPEECH_SAMPLE_H
