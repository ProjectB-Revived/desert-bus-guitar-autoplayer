#include "MidiFile.h"
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>

using namespace smf;

// Note mapping
struct NoteKey {
    int midiNote;
    WORD vKey;
};

// C4 to E5 (White keys only)
const std::vector<NoteKey> WhiteKeyMap = {
    {60, 0x5A}, // C4 - z
    {62, 0x58}, // D4 - x
    {64, 0x43}, // E4 - c
    {65, 0x56}, // F4 - v
    {67, 0x42}, // G4 - b
    {69, 0x4E}, // A4 - n
    {71, 0x4D}, // B4 - m
    {72, 0x4A}, // C5 - j
    {74, 0x4B}, // D5 - k
    {76, 0x4C}  // E5 - l
};

// Complex Mode Fret Keys (20 frets)
const std::vector<WORD> ComplexFretKeys = {
    0x45, 0x52, 0x54, 0x59, 0x55, 0x50, 0xBD, 0xBB, 0xDB, 0xDD, // E R T Y U P - = [ ]
    0x46, 0x47, 0x48, 0x4A, 0x4B, 0x4C, 0xBA, 0xDE, 0x4D, 0xBC  // F G H J K L ; ' M ,
};

// Complex Mode Play Keys (6 keys)
const std::vector<WORD> ComplexPlayKeys = {
    0x5A, 0x58, 0x43, 0x56, 0x42, 0x4E // Z X C V B N
};

bool isWhiteKey(int midiNote) {
    int note = midiNote % 12;
    return (note == 0 || note == 2 || note == 4 || note == 5 || note == 7 || note == 9 || note == 11);
}

void tapKeys(const std::vector<WORD>& vKeys) {
    if (vKeys.empty()) return;

    std::vector<INPUT> inputs;
    for (WORD vKey : vKeys) {
        UINT scanCode = MapVirtualKey(vKey, MAPVK_VK_TO_VSC);
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 0;
        input.ki.wScan = (WORD)scanCode;
        input.ki.dwFlags = KEYEVENTF_SCANCODE;
        inputs.push_back(input);
    }

    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    for (auto& input : inputs) {
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
    }
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
}

void tapKey(WORD vKey) {
    tapKeys({vKey});
}

std::string openFileDialog() {
    OPENFILENAMEA ofn;
    char szFile[260] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn)) {
        return std::string(szFile);
    }
    return "";
}

int main() {
    std::cout << "Desert Bus AutoPlayer" << std::endl;
    std::cout << "---------------------" << std::endl;

    std::string filePath = openFileDialog();
    if (filePath.empty()) {
        std::cout << "No file selected. Exiting." << std::endl;
        return 0;
    }

    bool complexMode = false;
    std::cout << "Enable Complex Mode? (y/n): ";
    char choice;
    std::cin >> choice;
    if (choice == 'y' || choice == 'Y') {
        complexMode = true;
        std::cout << "Complex Mode enabled." << std::endl;
    } else {
        std::cout << "Simple Mode enabled." << std::endl;
    }

    double speed = 1.0;
    std::cout << "Enter playback speed (e.g., 1.0 for normal, 2.0 for double): ";
    if (!(std::cin >> speed)) {
        speed = 1.0;
    }

    MidiFile midifile;
    if (!midifile.read(filePath)) {
        std::cerr << "Error reading MIDI file: " << filePath << std::endl;
        return 1;
    }

    midifile.doTimeAnalysis();
    midifile.linkNotePairs();
    midifile.joinTracks();

    std::vector<MidiEvent*> notes;
    int channelPrograms[16] = {0}; // Default to Acoustic Grand Piano

    for (int event = 0; event < midifile.getEventCount(0); event++) {
        MidiEvent& mev = midifile[0][event];
        if (mev.isPatchChange()) {
            channelPrograms[mev.getChannel()] = mev.getP1();
        } else if (mev.isNoteOn() && mev.getVelocity() > 0) {
            int channel = mev.getChannel();
            int program = channelPrograms[channel];

            // Filter: No drums (Channel 10, index 9)
            // Programs: Piano (0-7), Guitar (24-31)
            bool isPiano = (program >= 0 && program <= 7);
            bool isGuitar = (program >= 24 && program <= 31);

            if (channel != 9 && (isPiano || isGuitar)) {
                notes.push_back(&mev);
            }
        }
    }

    // Sort notes by time
    std::sort(notes.begin(), notes.end(), [](MidiEvent* a, MidiEvent* b) {
        return a->seconds < b->seconds;
    });

    std::cout << "Press ESC to start/stop playback." << std::endl;
    std::cout << "Waiting for ESC..." << std::endl;

    bool playing = false;
    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            // Wait for key release to avoid immediate toggle
            while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            playing = true;
            std::cout << "Playing..." << std::endl;
            auto startTime = std::chrono::steady_clock::now();
            size_t nextNoteIndex = 0;
            int currentFret = -1;

            while (playing && nextNoteIndex < notes.size()) {
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    // Wait for key release
                    while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    playing = false;
                    std::cout << "Stopped." << std::endl;
                    break;
                }

                auto currentTime = std::chrono::steady_clock::now();
                double elapsedSeconds = std::chrono::duration<double>(currentTime - startTime).count() * speed;

                while (nextNoteIndex < notes.size() && notes[nextNoteIndex]->seconds <= elapsedSeconds) {
                    std::vector<MidiEvent*> currentChord;
                    double chordStartTime = notes[nextNoteIndex]->seconds;
                    while (nextNoteIndex < notes.size() && notes[nextNoteIndex]->seconds <= chordStartTime + 0.010) {
                        currentChord.push_back(notes[nextNoteIndex]);
                        nextNoteIndex++;
                    }

                    if (complexMode) {
                        // Find the best fret for this chord
                        int bestFret = -1;
                        int maxPlayable = -1;

                        for (int f = 0; f < 20; f++) {
                            int playable = 0;
                            for (auto note : currentChord) {
                                int clamped = std::max(60, std::min(84, note->getKeyNumber()));
                                int idx = clamped - 60 - f;
                                if (idx >= 0 && idx < 6) playable++;
                            }
                            if (playable > maxPlayable) {
                                maxPlayable = playable;
                                bestFret = f;
                            } else if (playable == maxPlayable && currentFret != -1) {
                                if (f == currentFret) bestFret = f;
                                else if (bestFret != currentFret && std::abs(f - currentFret) < std::abs(bestFret - currentFret)) {
                                    bestFret = f;
                                }
                            }
                        }

                        if (bestFret != -1 && bestFret != currentFret) {
                            tapKey(ComplexFretKeys[bestFret]);
                            currentFret = bestFret;
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }

                        std::vector<WORD> keysToPlay;
                        for (auto note : currentChord) {
                            int clamped = std::max(60, std::min(84, note->getKeyNumber()));
                            int idx = clamped - 60 - currentFret;
                            if (idx >= 0 && idx < 6) {
                                keysToPlay.push_back(ComplexPlayKeys[idx]);
                            }
                        }
                        if (!keysToPlay.empty()) {
                            tapKeys(keysToPlay);
                        }
                    } else {
                        // Simple Mode: White keys only, C4 to E5
                        for (auto note : currentChord) {
                            int midiNote = note->getKeyNumber();
                            if (isWhiteKey(midiNote)) {
                                int clampedNote = std::max(60, std::min(76, midiNote));
                                WORD vKey = 0;
                                for (const auto& nk : WhiteKeyMap) {
                                    if (nk.midiNote == clampedNote) {
                                        vKey = nk.vKey;
                                        break;
                                    }
                                }
                                if (vKey != 0) {
                                    tapKey(vKey);
                                }
                            }
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (nextNoteIndex >= notes.size()) {
                std::cout << "Finished playback." << std::endl;
            }
            playing = false;
            std::cout << "Press ESC to start again." << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}
