#pragma once
// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <memory>

namespace optparse
{
class OptionParser;
class Values;
}

namespace CommandLineParse
{
enum class ParserOptions
{
  IncludeGUIOptions,
  OmitGUIOptions,
};

std::unique_ptr<optparse::OptionParser> CreateParser(ParserOptions options);
optparse::Values& ParseArguments(optparse::OptionParser* parser, int argc, char** argv);
}
