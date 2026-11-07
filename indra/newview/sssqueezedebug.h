/**
 * @file sssqueezedebug.h
 * @brief Squeeze P0 self test - synthetic BC7 upload and VRAM accounting proof, see doc/viewer/super_compressed_textures.md
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Soapstorm Viewer Source Code
 * $/LicenseInfo$
 */

#ifndef SS_SQUEEZEDEBUG_H
#define SS_SQUEEZEDEBUG_H

// Uploads a synthetic full mip chain BC7 texture through the ordinary LLImageGL path, re-uploads it into the same texture name, then replaces it with an uncompressed image, logging GL errors and the change in LLImageGL::getTextureBytesAllocated() at each step.
void ss_squeeze_self_test();

// Re-reads SSSqueezeEnabled into LLImageGL::sSqueezeEnabled so the gate can be flipped without a restart.
void ss_squeeze_refresh_enabled();

#endif // SS_SQUEEZEDEBUG_H
