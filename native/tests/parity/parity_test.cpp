// parity_test.cpp - exercises the ported C++ LUT core against fixed inputs so the
// TS oracle (src/core/lut/cube.ts) can diff its output. Compiled by
// scripts/parity-test.mjs with the host g++/clang, no AE SDK required.
//
// Usage: parity_test <lut.cube> <inputs.txt>
//   inputs.txt: one "r g b" triple per line (floats).
//   stdout:     one "r g b" triple per line, the sampleLut result, 8 decimals.
#include "../../ColorGradeFX/lut/CubeLut.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <lut.cube> <inputs.txt>\n", argv[0]);
        return 2;
    }

    std::ifstream lutFile(argv[1]);
    if (!lutFile) {
        std::fprintf(stderr, "parity_test: cannot open cube %s\n", argv[1]);
        return 2;
    }
    std::stringstream lutBuf;
    lutBuf << lutFile.rdbuf();

    cg::Lut3D lut;
    try {
        lut = cg::parseCube(lutBuf.str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parity_test: %s\n", e.what());
        return 2;
    }

    std::ifstream inputs(argv[2]);
    if (!inputs) {
        std::fprintf(stderr, "parity_test: cannot open inputs %s\n", argv[2]);
        return 2;
    }

    std::string line;
    while (std::getline(inputs, line)) {
        std::istringstream ls(line);
        float r, g, b;
        if (!(ls >> r >> g >> b)) continue;
        cg::Vec3 out = cg::sampleLut(lut, {r, g, b});
        std::printf("%.8f %.8f %.8f\n", out[0], out[1], out[2]);
    }
    return 0;
}
