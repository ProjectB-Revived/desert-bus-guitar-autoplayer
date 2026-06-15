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
#include <set>

using namespace smf;

// Note mapping
struct NoteKey {
    int midiNote;
    WORD vKey;
};

// C4 to E5 (White keys only) - Simple Mode
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
// Each fret shifts the 6 play keys up by 1 semitone.
// Fret 0 = C4 (MIDI 60) through fret 19 = B5 (MIDI 83)
const std::vector<WORD> ComplexFretKeys = {
    0x45, 0x52, 0x54, 0x59, 0x55, 0x50, 0xBD, 0xBB, 0xDB, 0xDD, // E R T Y U P - = [ ]
    0x46, 0x47, 0x48, 0x4A, 0x4B, 0x4C, 0xBA, 0xDE, 0x4D, 0xBC  // F G H J K L ; ' M ,
};

// Complex Mode Play Keys (6 strings: Z X C V B N)
const std::vector<WORD> ComplexPlayKeys = {
    0x5A, 0x58, 0x43, 0x56, 0x42, 0x4E // Z X C V B N
};

// Each fret covers 6 consecutive semitones starting at MIDI note (60 + fret).
// fret 0  -> MIDI 60..65  (C4 D4 E4 F4 G4 Ab4)
// fret 1  -> MIDI 61..66  (Db4 D4 E4 F4 G4 A4)
// ...
// fret 19 -> MIDI 79..84  (G5 Ab5 A5 Bb5 B5 C6)
static const int FRET_BASE_MIDI = 60; // MIDI note that fret 0, string 0 plays
static const int NUM_FRETS      = 20;
static const int NUM_STRINGS    = 6;

bool isWhiteKey(int midiNote) {
    int note = midiNote % 12;
    return (note == 0 || note == 2 || note == 4 || note == 5 ||
            note == 7 || note == 9 || note == 11);
}

// Press all keys in the list simultaneously using a single SendInput call,
// hold for ~50ms, then release them all simultaneously.
void tapKeys(const std::vector<WORD>& vKeys) {
    if (vKeys.empty()) return;

    // Build key-down events
    std::vector<INPUT> inputs;
    inputs.reserve(vKeys.size() * 2);
    for (WORD vKey : vKeys) {
        INPUT input = {};
        input.type        = INPUT_KEYBOARD;
        input.ki.wVk      = vKey;
        input.ki.wScan    = (WORD)MapVirtualKey(vKey, MAPVK_VK_TO_VSC);
        input.ki.dwFlags  = 0; // key down
        inputs.push_back(input);
    }

    // Send all key-downs in one call → simultaneous press
    SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Build and send key-up events in one call → simultaneous release
    std::vector<INPUT> upInputs;
    upInputs.reserve(vKeys.size());
    for (WORD vKey : vKeys) {
        INPUT input = {};
        input.type        = INPUT_KEYBOARD;
        input.ki.wVk      = vKey;
        input.ki.wScan    = (WORD)MapVirtualKey(vKey, MAPVK_VK_TO_VSC);
        input.ki.dwFlags  = KEYEVENTF_KEYUP;
        upInputs.push_back(input);
    }
    SendInput((UINT)upInputs.size(), upInputs.data(), sizeof(INPUT));
}

void tapKey(WORD vKey) {
    tapKeys({vKey});
}

// -----------------------------------------------------------------------
// Complex Mode chord playback
//
// Given a chord (set of MIDI notes), choose the fret that lets us play
// the most notes. Prefer the fret closest to the current one when tied.
// Then press the fret key (if it changed) and all matching string keys
// simultaneously.
// -----------------------------------------------------------------------
void playChordComplex(const std::vector<int>& midiNotes, int& currentFret) {

    // Clamp notes into the 20-fret range [60, 84]
    std::vector<int> clamped;
    for (int n : midiNotes) {
        clamped.push_back(std::max(FRET_BASE_MIDI,
                           std::min(FRET_BASE_MIDI + NUM_FRETS + NUM_STRINGS - 2, n)));
    }

    // Find the fret that covers the most notes.
    // Fret f covers MIDI notes [FRET_BASE_MIDI+f, FRET_BASE_MIDI+f+NUM_STRINGS-1]
    int bestFret    = (currentFret == -1) ? 0 : currentFret;
    int maxCovered  = -1;

    for (int f = 0; f < NUM_FRETS; f++) {
        int lo = FRET_BASE_MIDI + f;
        int hi = lo + NUM_STRINGS - 1;
        int covered = 0;
        for (int n : clamped) {
            if (n >= lo && n <= hi) covered++;
        }
        if (covered > maxCovered ||
            (covered == maxCovered &&
             std::abs(f - currentFret) < std::abs(bestFret - currentFret))) {
            maxCovered = covered;
            bestFret   = f;
        }
    }

    // Change fret if needed. Press the fret key and wait for it to register
    // BEFORE pressing string keys.
    if (bestFret != currentFret) {
        tapKey(ComplexFretKeys[bestFret]);
        currentFret = bestFret;
        // Small pause so the game registers the fret change before string press
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // Build list of string keys to press simultaneously
    // Use a set to avoid duplicates (two MIDI notes mapping to same string)
    std::set<int> stringIndices;
    int lo = FRET_BASE_MIDI + currentFret;
    for (int n : clamped) {
        int idx = n - lo;
        if (idx >= 0 && idx < NUM_STRINGS) {
            stringIndices.insert(idx);
        }
    }

    std::vector<WORD> keysToPlay;
    for (int idx : stringIndices) {
        keysToPlay.push_back(ComplexPlayKeys[idx]);
    }

    if (!keysToPlay.empty()) {
        tapKeys(keysToPlay); // all pressed in ONE SendInput call → truly simultaneous
    }
}

std::string openFileDialog() {
    OPENFILENAMEA ofn;
    char szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = NULL;
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = sizeof(szFile);
    ofn.lpstrFilter  = "MIDI Files\0*.mid;*.midi\0All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(szFile);
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
    std::cout << "Enter playback speed (e.g., 1.0 for normal, 0.5 for half): ";
    if (!(std::cin >> speed) || speed <= 0.0) speed = 1.0;

    MidiFile midifile;
    if (!midifile.read(filePath)) {
        std::cerr << "Error reading MIDI file: " << filePath << std::endl;
        return 1;
    }

    midifile.doTimeAnalysis();
    midifile.linkNotePairs();
    midifile.joinTracks();

    std::vector<MidiEvent*> notes;
    int channelPrograms[16] = {0};

    for (int event = 0; event < midifile.getEventCount(0); event++) {
        MidiEvent& mev = midifile[0][event];
        if (mev.isPatchChange()) {
            channelPrograms[mev.getChannel()] = mev.getP1();
        } else if (mev.isNoteOn() && mev.getVelocity() > 0) {
            int channel = mev.getChannel();
            int program = channelPrograms[channel];
            bool isPiano  = (program >= 0  && program <= 7);
            bool isGuitar = (program >= 24 && program <= 31);
            if (channel != 9 && (isPiano || isGuitar)) {
                notes.push_back(&mev);
            }
        }
    }

    std::sort(notes.begin(), notes.end(), [](MidiEvent* a, MidiEvent* b) {
        return a->seconds < b->seconds;
    });

    std::cout << "Loaded " << notes.size() << " notes." << std::endl;
    std::cout << "Press ESC to start/stop playback." << std::endl;
    std::cout << "Waiting for ESC..." << std::endl;

    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            while (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));

            bool playing = true;
            std::cout << "Playing..." << std::endl;
            auto startTime   = std::chrono::steady_clock::now();
            size_t noteIdx   = 0;
            int currentFret  = -1;  // -1 = no fret pressed yet

            while (playing && noteIdx < notes.size()) {
                // ESC to stop
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    while (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    playing = false;
                    std::cout << "Stopped." << std::endl;
                    break;
                }

                double elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - startTime).count() * speed;

                // Fire all chords whose timestamp has arrived
                while (noteIdx < notes.size() && notes[noteIdx]->seconds <= elapsed) {
                    // Gather all notes within 10ms of this one → one chord
                    double chordTime = notes[noteIdx]->seconds;
                    std::vector<int> chordNotes;
                    while (noteIdx < notes.size() &&
                           notes[noteIdx]->seconds <= chordTime + 0.010) {
                        chordNotes.push_back(notes[noteIdx]->getKeyNumber());
                        noteIdx++;
                    }

                    if (complexMode) {
                        // FIX: plays all chord notes simultaneously
                        playChordComplex(chordNotes, currentFret);
                    } else {
                        // Simple Mode: collect all matching keys, press together
                        std::vector<WORD> keysToPlay;
                        for (int midiNote : chordNotes) {
                            if (!isWhiteKey(midiNote)) continue;
                            int clamped = std::max(60, std::min(76, midiNote));
                            for (const auto& nk : WhiteKeyMap) {
                                if (nk.midiNote == clamped) {
                                    keysToPlay.push_back(nk.vKey);
                                    break;
                                }
                            }
                        }
                        if (!keysToPlay.empty()) tapKeys(keysToPlay);
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (noteIdx >= notes.size()) {
                std::cout << "Finished playback." << std::endl;
            }
            std::cout << "Press ESC to play again." << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return 0;
}
