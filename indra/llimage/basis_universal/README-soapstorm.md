# Basis Universal transcoder, vendored for Squeeze

This directory is an UNMODIFIED copy of the `transcoder/` directory of Binomial LLC's Basis
Universal, plus its `LICENSE` and `NOTICE`, taken from
https://github.com/BinomialLLC/basis_universal at commit `99f52d63aa6799cbdaecfe977111dc5ec3b31d47`
(master, after tag `v2_50`) on 2026-09-09. This file is the only thing here that Soapstorm wrote.

Why it is here: Squeeze's BC7 block backend is `bc7f`, Binomial's analytical one-shot BC7 encoder,
which lives inside `transcoder/basisu_transcoder.cpp` in the `basist::bc7f` namespace and cannot be
separated from that translation unit without editing an upstream file. So the whole transcoder is
compiled as one separate component, reached only through its public header from
`indra/llimage/ssbc7block_bc7f.cpp`. Nothing else in the viewer includes it.

What is deliberately NOT here: the encoder library, the command line tool, `zstd/` (BSD 3-Clause,
compiled out with `BASISD_SUPPORT_KTX2_ZSTD=0`), and `encoder/3rdparty/` (MIT and BSD parts). Per
upstream's `.reuse/dep5`, everything under `transcoder/` is Binomial's own work under the Apache
License 2.0.

Rules, from `doc/super_compressed_textures.md`:

- Do not edit any file under `transcoder/`. If an upstream fix is ever needed, take a new upstream
  commit and update the hash above. Editing a file would trigger Apache 2.0 section 4(b) and break
  the "separate, unmodified component" posture the licensing argument rests on.
- Do not copy any of this source into a file carrying the Soapstorm licence header.
- The notices in `NOTICE` are reproduced in `indra/newview/licenses-*.txt`; keep them in step if
  upstream's change.
- `SS_BC7F=OFF` at configure time builds the viewer with none of this code in it, using the
  portable mode 6 backend instead.
