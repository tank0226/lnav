/**
 * Copyright (c) 2023, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef piper_looper_cfg_hh
#define piper_looper_cfg_hh

#include <chrono>
#include <map>
#include <string>

#include <stdint.h>

#include "base/file_range.hh"
#include "pcrepp/pcre2pp.hh"
#include "yajlpp/yajlpp.hh"

namespace lnav::piper {

struct demux_def {
    bool dd_enabled{true};
    bool dd_valid{false};
    factory_container<pcre2pp::code> dd_control_pattern;
    factory_container<pcre2pp::code> dd_pattern;
    int dd_timestamp_capture_index{-1};
    int dd_muxid_capture_index{-1};
    int dd_body_capture_index{-1};
    std::map<std::string, int> dd_meta_capture_indexes;
};

struct demux_json_def {
    bool djd_enabled{true};
    std::string djd_mux_id;
    std::string djd_body;
    std::string djd_timestamp;
};

struct config {
    file_off_t c_max_size{10LL * 1024LL * 1024LL};
    uint32_t c_rotations{4};
    std::chrono::seconds c_ttl{std::chrono::hours(48)};

    std::map<std::string, demux_def> c_demux_definitions;
    std::map<std::string, demux_json_def> c_demux_json_definitions;
};

}  // namespace lnav::piper

#endif
