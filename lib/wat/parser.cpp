// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2019-2026 Second State INC

#include "wat/parser.h"
#include "converter.h"
#include "tree_sitter.h"

#include <fstream>
#include <iterator>
#include <string>

extern "C" const TSLanguage *tree_sitter_wat();

namespace WasmEdge::WAT {

Expect<AST::Module> parseWat(std::string_view Source, const Configure &Conf) {
  Parser P(tree_sitter_wat);
  Tree T = P.parse(Source);
  if (T.rootNode().isNull()) {
    return Unexpect(ErrCode::Value::WatUnexpectedEnd);
  }
  Converter Conv(Conf);
  return Conv.convert(T, Source);
}

Expect<AST::Module> parseWatFile(const std::filesystem::path &Path,
                                 const Configure &Conf) {
  // Binary mode preserves the exact on-disk bytes (no CRLF->LF translation on
  // Windows), keeping tree-sitter's byte offsets aligned with the file.
  std::ifstream Ifs(Path, std::ios::binary);
  if (!Ifs) {
    return Unexpect(ErrCode::Value::IllegalPath);
  }
  std::string Source((std::istreambuf_iterator<char>(Ifs)),
                     std::istreambuf_iterator<char>());
  return parseWat(Source, Conf);
}

} // namespace WasmEdge::WAT
