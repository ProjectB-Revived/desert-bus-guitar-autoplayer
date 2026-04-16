#include "MidiFile.h"
#include <windows.h>
#include <commdlg.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>

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

void tapKey(WORD vKey) {
    UINT scanCode = MapVirtualKey(vKey, MAPVK_VK_TO_VSC);

    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0;
    input.ki.wScan = (WORD)scanCode;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;

    SendInput(1, &input, sizeof(INPUT));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
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

    std::vector<MidiEvent*> notes;
    for (int track = 0; track < midifile.getTrackCount(); track++) {
        for (int event = 0; event < midifile.getEventCount(track); event++) {
            if (midifile[track][event].isNoteOn()) {
                notes.push_back(&midifile[track][event]);
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
                    int midiNote = notes[nextNoteIndex]->getKeyNumber();

                    if (complexMode) {
                        // Complex Mode: Chromatic, 20 frets + 6 strings
                        // Range: 60 (C4) to 60 + 19 + 5 = 84 (C6)
                        int clampedNote = midiNote;
                        if (clampedNote < 60) clampedNote = 60;
                        if (clampedNote > 84) clampedNote = 84;

                        int fretNeeded = -1;
                        // If current fret can play this note, keep it
                        if (currentFret != -1) {
                            int playIndex = clampedNote - 60 - currentFret;
                            if (playIndex >= 0 && playIndex < 6) {
                                fretNeeded = currentFret;
                            }
                        }

                        // Otherwise, find a fret that can play this note (try to center it)
                        if (fretNeeded == -1) {
                            fretNeeded = clampedNote - 60 - 2;
                            if (fretNeeded < 0) fretNeeded = 0;
                            if (fretNeeded > 19) fretNeeded = 19;

                            // Re-verify if this fret can actually play the note
                            int playIndex = clampedNote - 60 - fretNeeded;
                            if (playIndex < 0 || playIndex >= 6) {
                                // Fallback: just use the lowest possible fret that works
                                fretNeeded = clampedNote - 60 - 5;
                                if (fretNeeded < 0) fretNeeded = 0;
                            }
                        }

                        if (fretNeeded != currentFret) {
                            tapKey(ComplexFretKeys[fretNeeded]);
                            currentFret = fretNeeded;
                            // Small delay after fret change to ensure game registers it
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }

                        int playIndex = clampedNote - 60 - currentFret;
                        if (playIndex >= 0 && playIndex < 6) {
                            tapKey(ComplexPlayKeys[playIndex]);
                        }
                    } else {
                        // Simple Mode: White keys only, C4 to E5
                        if (isWhiteKey(midiNote)) {
                            int clampedNote = midiNote;
                            if (clampedNote < 60) clampedNote = 60;
                            if (clampedNote > 76) clampedNote = 76;

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
                    nextNoteIndex++;
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
