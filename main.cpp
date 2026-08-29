// This file is a harness I wrote around the real Monero decoy selection
// algorithm - not upstream code itself. It talks to a node's RPC, feeds
// gamma_picker (ported verbatim from wallet2, see gamma_picker.h/.cpp),
// verifies each candidate against /get_outs, and skips locked outputs,
// same as wallet2::get_outs() does.
//
// Lock status comes straight from the daemon ("unlocked" field in
// /get_outs; see src/rpc/core_rpc_server.cpp on_get_outs()). We don't
// re-derive it client-side (CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE, the
// coinbase CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW, etc.) - wallet2 trusts
// the daemon for this too.
//
// === Segregation (pre-fork / post-fork) support ===
// Source: monero-project/monero src/wallet/wallet2.cpp, wallet2::get_outs()
// (RCT / amount==0 branch, around line 9015-9620), and the constants at
// line 121-142 of the same file.
//
// SEGREGATION_FORK_HEIGHT, TESTNET_SEGREGATION_FORK_HEIGHT and
// STAGENET_SEGREGATION_FORK_HEIGHT are all defined as 99999999 upstream
// (wallet2.cpp line 139-141) - a sentinel for a fork that never happened.
// A stock wallet2 only uses something else if the user explicitly sets
// segregation-height. Since real chain heights are nowhere near that
// sentinel, is_after_segregation_fork is false by default and this
// whole branch never runs unless segregation-height is passed in.
//
// So: with no --segregation-height, this behaves exactly like a plain
// single-pool gamma pick. With --segregation-height N, it reproduces
// wallet2's two-phase process:
//
//   Phase 1 (draw): a fixed-size candidate pool (REQUESTED_POOL_SIZE=75)
//   is drawn in order - pre-fork range first, then post-fork, then the
//   rest of the chain. Lock status is unknown at this point (wallet2.cpp
//   line 9370-9456: the draw loop runs until it has 75 candidates, not
//   until it has 15).
//
//   Phase 2 (fill): the whole pool's lock status is queried in one
//   batch. The pool is then scanned in order, locked entries
//   are skipped with no replacement, and the scan stops once 15 valid
//   decoys are collected (wallet2.cpp ~line 9600, the
//   outs.back().size() < fake_outputs_count+1 loop condition). If the
//   pool runs out before reaching 15, this fails loudly instead of
//   silently drawing more from whichever bucket came up short - that's
//   what lets the pre-fork/post-fork/normal ratio shift under load the
//   same way it does in wallet2, instead of staying artificially fixed.

#include <iostream>
#include <vector>
#include <set>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "gamma_picker.h"

using json = nlohmann::json;

// --- Segregation constants (wallet2.cpp line 121-142) ---
#define RECENT_OUTPUT_RATIO 0.5                          // line 121
#define RECENT_OUTPUT_DAYS 1.8                            // line 122
#define RECENT_OUTPUT_BLOCKS (RECENT_OUTPUT_DAYS * 720)   // line 124
#define SEGREGATION_FORK_VICINITY 1500ULL                 // line 142
// wallet2.cpp line 139-141: mainnet/testnet/stagenet all use the same
// sentinel - "high enough to never trigger by default". wallet2 uses
// m_segregation_height if it's set, otherwise falls back to this (see
// wallet2::get_segregation_fork_height()).
#define SEGREGATION_FORK_HEIGHT_SENTINEL 99999999ULL

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// rpc_url example: http://127.0.0.1:38081
static json rpc_call(const std::string &rpc_url, const std::string &method, const json &params) {
    CURL *curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");

    json body = {
        {"jsonrpc", "2.0"},
        {"id", "0"},
        {"method", method},
        {"params", params}
    };
    std::string body_str = body.dump();
    std::string response_str;

    std::string url = rpc_url + "/json_rpc";
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_str);
    // rct_offsets can be huge (hundreds of thousands to millions of
    // values). A low-speed cutoff instead of a flat timeout so large but
    // slowly-progressing transfers don't get killed mid-stream.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // 1KB/s floor
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);    // over 30s
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // Request gzip/deflate at the HTTP level - the distribution array is
    // mostly repeated/monotonically increasing numbers, so it compresses
    // well, which also cuts down on partial-transfer/timeout risk.
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    // DECOY_DEBUG=1 dumps curl's full handshake/header/transfer trace to
    // stderr - useful for finding the actual cause of a partial-transfer
    // error (proxy limits, TLS issues, truncated chunked encoding, etc).
    if (std::getenv("DECOY_DEBUG")) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_off_t size_download = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &size_download);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("RPC request failed: ") + curl_easy_strerror(res)
            + " (bytes received: " + std::to_string(size_download)
            + ", first 200 chars of response: " + response_str.substr(0, 200) + ")");
    }
    if (http_code != 200) {
        throw std::runtime_error("RPC HTTP " + std::to_string(http_code) + ": " + response_str);
    }

    json j = json::parse(response_str);
    if (j.contains("error")) {
        throw std::runtime_error("RPC error: " + j["error"].dump());
    }
    return j["result"];
}

// Lightweight call to get the current chain tip.
static uint64_t fetch_chain_height(const std::string &rpc_url) {
    json result = rpc_call(rpc_url, "get_info", json::object());
    if (!result.contains("height")) {
        throw std::runtime_error("get_info response has no 'height' field");
    }
    return result["height"].get<uint64_t>();
}

// Calls get_output_distribution for a single [from_height, to_height]
// window. With cumulative=true the returned 'distribution' values are
// ABSOLUTE cumulative counts from genesis, not relative to the window
// (see src/rpc/rpc_handler.cpp: process_distribution - no transform
// happens when cumulative=true, the raw absolute value is returned as
// is). So windows can just be concatenated; no extra base offset needed.
struct chunk_result { uint64_t start_height; std::vector<uint64_t> values; };

static chunk_result fetch_chunk(const std::string &rpc_url, uint64_t from_height, uint64_t to_height) {
    json params = {
        {"amounts", json::array({0})},
        {"from_height", from_height},
        {"to_height", to_height},
        {"cumulative", true},
        {"binary", false},
        {"compress", false}
    };
    json result = rpc_call(rpc_url, "get_output_distribution", params);

    if (!result.contains("distributions") || result["distributions"].empty()) {
        throw std::runtime_error("empty 'distributions' field");
    }
    auto &d = result["distributions"][0];
    if (d["amount"].get<uint64_t>() != 0) {
        throw std::runtime_error("got a result for amount != 0 (non-RingCT)");
    }
    chunk_result cr;
    cr.start_height = d.value("start_height", from_height);
    cr.values = d["distribution"].get<std::vector<uint64_t>>();
    return cr;
}

// Produces the same result as wallet2::get_rct_distribution() (an
// absolute cumulative rct_offsets vector), but fetches it in small
// chunks instead of one large request, to avoid the large-response
// truncation issue (daemons sometimes return "Transferred a partial
// file" on very large single requests). If a chunk still gets
// truncated, it's automatically retried at half the size.
static std::vector<uint64_t> fetch_rct_offsets(const std::string &rpc_url, uint64_t chain_height) {
    uint64_t chain_tip = chain_height - 1;

    std::vector<uint64_t> offsets;
    uint64_t from_height = 1220516;
    uint64_t real_start_height = 0;
    bool first_chunk = true;
    uint64_t chunk_size = 100000; // initial chunk size (blocks)

    while (from_height <= chain_tip) {
        uint64_t to_height = std::min(from_height + chunk_size - 1, chain_tip);

        chunk_result cr;
        bool ok = false;
        uint64_t try_size = chunk_size;
        for (int attempt = 0; attempt < 6 && !ok; ++attempt) {
            uint64_t this_to = std::min(from_height + try_size - 1, chain_tip);
            try {
                cr = fetch_chunk(rpc_url, from_height, this_to);
                to_height = this_to;
                ok = true;
            } catch (const std::exception &e) {
                try_size = std::max<uint64_t>(try_size / 2, 1000);
                std::cerr << "  chunk failed (" << e.what() << "), reducing chunk size to "
                          << try_size << ", retrying...\n";
            }
        }
        if (!ok) {
            throw std::runtime_error("a chunk failed after 6 attempts (from_height=" + std::to_string(from_height) + ")");
        }

        if (first_chunk) {
            real_start_height = cr.start_height;
            first_chunk = false;
        }

        offsets.insert(offsets.end(), cr.values.begin(), cr.values.end());

        std::cerr << "  fetched [" << from_height << "-" << to_height << "] ("
                  << cr.values.size() << " blocks, " << offsets.size() << " total)\n";

        from_height = to_height + 1;
        chunk_size = 100000; // back to the normal size for the next chunk
    }

    if (real_start_height != 0) {
        std::cerr << "Note: RCT outputs start at height " << real_start_height
                  << " on this chain (no earlier blocks, this is expected).\n";
    }

    return offsets;
}

// --- Segregation: cumulative distribution around the fork ---
// Source: wallet2.cpp line 9081-9120 (the get_output_distribution call
// made when m_segregate_pre_fork_outputs || m_key_reuse_mitigation2).
// from_height = max(fork, RECENT_OUTPUT_BLOCKS) - RECENT_OUTPUT_BLOCKS
// to_height   = fork + 1
// till_fork   = distribution[fork - start_height]
// recent      = till_fork - distribution[fork - RECENT_OUTPUT_BLOCKS - start_height]
struct segregation_limit_t {
    uint64_t till_fork = 0;
    uint64_t recent = 0;
};

static segregation_limit_t fetch_segregation_limit(const std::string &rpc_url, uint64_t segregation_fork_height) {
    const uint64_t recent_blocks = (uint64_t)RECENT_OUTPUT_BLOCKS;
    uint64_t from_height = std::max<uint64_t>(segregation_fork_height, recent_blocks) - recent_blocks;
    uint64_t to_height = segregation_fork_height + 1;

    std::cerr << "Fetching segregation window: [" << from_height << ", " << to_height << "]...\n";
    chunk_result cr = fetch_chunk(rpc_url, from_height, to_height);
    uint64_t start_height = cr.start_height;

    // same guards as wallet2.cpp line 9112-9116
    if (start_height > segregation_fork_height)
        throw std::runtime_error("segregation: distribution start_height too high");
    if (segregation_fork_height - start_height >= cr.values.size())
        throw std::runtime_error("segregation: distribution too small (fork)");
    if (segregation_fork_height < recent_blocks)
        throw std::runtime_error("segregation: fork height too low");
    if (segregation_fork_height - recent_blocks - start_height >= cr.values.size())
        throw std::runtime_error("segregation: distribution too small (recent)");
    if (segregation_fork_height - recent_blocks < start_height)
        throw std::runtime_error("segregation: bad start height");

    segregation_limit_t lim;
    lim.till_fork = cr.values[segregation_fork_height - start_height];
    lim.recent = lim.till_fork - cr.values[segregation_fork_height - recent_blocks - start_height];
    return lim;
}

// Generic POST helper for the plain REST-style endpoints (like get_outs)
// that don't go through the /json_rpc wrapper. Uses the same timeout/
// retry settings as rpc_call.
static json direct_post(const std::string &rpc_url, const std::string &endpoint, const json &body) {
    CURL *curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");

    std::string body_str = body.dump();
    std::string response_str;
    std::string url = rpc_url + endpoint;

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_str);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (std::getenv("DECOY_DEBUG")) {
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("RPC request failed (") + endpoint + "): " + curl_easy_strerror(res));
    }
    if (http_code != 200) {
        throw std::runtime_error(endpoint + " HTTP " + std::to_string(http_code) + ": " + response_str);
    }
    return json::parse(response_str);
}

// Fetches the real public key, mask, and - most importantly - the
// "unlocked" (currently spendable) status for a batch of global indices
// (amount=0 / RingCT). Source: src/rpc/core_rpc_server.cpp
// on_get_outs() + COMMAND_RPC_GET_OUTPUTS (in
// src/rpc/core_rpc_server_commands_defs.h). The "unlocked" field is
// computed daemon-side (accounting for
// CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE and, for coinbase outputs,
// CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW) - we don't recompute it, we
// trust the daemon directly, same as wallet2.
struct out_info {
    std::string key;
    std::string mask;
    bool unlocked;
    uint64_t height;
};

static std::vector<out_info> fetch_outs(const std::string &rpc_url, const std::vector<uint64_t> &indices) {
    json outputs = json::array();
    for (auto idx : indices) {
        outputs.push_back({{"amount", 0}, {"index", idx}});
    }
    json body = {{"outputs", outputs}, {"get_txid", false}};
    json result = direct_post(rpc_url, "/get_outs", body);

    if (result.contains("error")) {
        throw std::runtime_error("/get_outs error: " + result["error"].dump());
    }
    if (!result.contains("outs")) {
        throw std::runtime_error("/get_outs response has no 'outs' field");
    }
    std::vector<out_info> outs;
    for (auto &o : result["outs"]) {
        out_info oi;
        oi.key = o.value("key", "");
        oi.mask = o.value("mask", "");
        oi.unlocked = o.value("unlocked", false);
        oi.height = o.value("height", 0);
        outs.push_back(oi);
    }
    if (outs.size() != indices.size()) {
        throw std::runtime_error("/get_outs returned an unexpected number of results ("
            + std::to_string(outs.size()) + " / " + std::to_string(indices.size()) + ")");
    }
    return outs;
}

// Gamma draw bounded to [lo, hi). Source: wallet2.cpp line 9374-9390 -
// the "do i = gamma->pick(); while (condition);" rejection-sampling
// pattern. gp.pick() already returns uint64_t max on a bad pick (see
// gamma_picker.cpp); we just skip those too.
static uint64_t bounded_gamma_pick(gamma_picker &gp, uint64_t lo, uint64_t hi) {
    const size_t MAX_ATTEMPTS = 200000;
    for (size_t a = 0; a < MAX_ATTEMPTS; ++a) {
        uint64_t i = gp.pick();
        if (i == std::numeric_limits<uint64_t>::max()) continue; // bad pick, same as wallet2.cpp
        if (i >= lo && i < hi) return i;
    }
    throw std::runtime_error("bounded_gamma_pick: could not find a valid index in ["
        + std::to_string(lo) + "," + std::to_string(hi) + ") within a reasonable number of attempts");
}

enum class slot_type { NORMAL, PRE_FORK, POST_FORK };

int main(int argc, char **argv) {
    // --- parse CLI args ---
    // Segregation flags use the same names as their wallet2.h
    // counterparts: segregate_pre_fork_outputs() / key_reuse_mitigation2()
    // / segregation_height(). Both default to true in the wallet2
    // constructor (line 1233-1235).
    uint64_t segregation_height_arg = 0; // 0 = "not specified" (same as wallet2.cpp), falls back to the sentinel
    bool segregate_pre_fork_flag = true;
    bool key_reuse_mitigation2_flag = true;

    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--segregation-height=", 0) == 0) {
            segregation_height_arg = std::stoull(a.substr(strlen("--segregation-height=")));
        } else if (a == "--no-segregate-pre-fork") {
            segregate_pre_fork_flag = false;
        } else if (a == "--no-key-reuse-mitigation2") {
            key_reuse_mitigation2_flag = false;
        } else {
            positional.push_back(a);
        }
    }

    if (positional.size() < 2) {
        std::cerr << "Usage: " << argv[0] << " <rpc_url> <real_global_index> [index2] [index3] ...\n";
        std::cerr << "       [--segregation-height=N] [--no-segregate-pre-fork] [--no-key-reuse-mitigation2]\n";
        std::cerr << "Example (single):   " << argv[0] << " http://127.0.0.1:38081 1234567\n";
        std::cerr << "Example (multiple): " << argv[0] << " http://127.0.0.1:38081 1234567 2345678\n";
        std::cerr << "\nWithout --segregation-height, wallet2.cpp's sentinel (99999999) is used,\n";
        std::cerr << "meaning segregation is effectively OFF (see wallet2::get_segregation_fork_height()) -\n";
        std::cerr << "this is also the default behavior of a real mainnet/testnet/stagenet wallet2.\n";
        return 1;
    }

    std::string rpc_url = positional[0];
    std::vector<uint64_t> real_indices;
    for (size_t i = 1; i < positional.size(); ++i) real_indices.push_back(std::stoull(positional[i]));

    uint64_t segregation_fork_height = segregation_height_arg > 0 ? segregation_height_arg : SEGREGATION_FORK_HEIGHT_SENTINEL;

    std::cerr << "Fetching chain height...\n";
    uint64_t chain_height = fetch_chain_height(rpc_url);
    std::cerr << "Chain height: " << chain_height << "\n";

    const bool is_after_segregation_fork = chain_height >= segregation_fork_height;
    const bool is_shortly_after_segregation_fork = is_after_segregation_fork &&
        chain_height < segregation_fork_height + SEGREGATION_FORK_VICINITY;

    if (segregation_height_arg == 0) {
        std::cerr << "Segregation: no --segregation-height given -> using the sentinel (" << SEGREGATION_FORK_HEIGHT_SENTINEL
                  << ") -> is_after_segregation_fork=false -> segregation OFF (matches stock wallet2).\n\n";
    } else {
        std::cerr << "Segregation: fork_height=" << segregation_fork_height
                  << ", is_after_segregation_fork=" << (is_after_segregation_fork ? "true" : "false")
                  << ", is_shortly_after=" << (is_shortly_after_segregation_fork ? "true" : "false") << "\n\n";
    }

    std::cerr << "Fetching rct_offsets (" << rpc_url << ")...\n";
    std::vector<uint64_t> rct_offsets = fetch_rct_offsets(rpc_url, chain_height);
    std::cerr << "Got distribution data for " << rct_offsets.size() << " blocks.\n";

    gamma_picker gp(rct_offsets);
    std::cerr << "num_rct_outputs (unlocked) = " << gp.get_num_rct_outs() << "\n\n";

    // The segregation window is only fetched when it's actually needed
    // (past the fork, and at least one of the two flags is on) - same
    // condition as wallet2.cpp line 9082.
    segregation_limit_t seg_limit;
    if (is_after_segregation_fork && (segregate_pre_fork_flag || key_reuse_mitigation2_flag)) {
        seg_limit = fetch_segregation_limit(rpc_url, segregation_fork_height);
        std::cerr << "Segregation limit: till_fork=" << seg_limit.till_fork
                  << ", recent=" << seg_limit.recent << "\n\n";
    }

    json sources_json = json::array();

    for (uint64_t real_index : real_indices) {
    std::cerr << "===== Source: global_index=" << real_index << " =====\n";
    if (real_index >= gp.get_num_rct_outs()) {
        std::cerr << "WARNING: the global index you gave (" << real_index
                  << ") is >= the daemon's unlocked output count (" << gp.get_num_rct_outs()
                  << "). Is this a very recent output that hasn't unlocked yet?\n";
    }

    // --- confirm the real output itself is actually spendable right now ---
    auto real_check = fetch_outs(rpc_url, {real_index});
    if (!real_check[0].unlocked) {
        std::cerr << "\n!!! WARNING: the REAL output you gave (index=" << real_index
                  << ") is NOT unlocked according to the daemon !!!\n"
                  << "    (height=" << real_check[0].height << "). Double check this index - "
                  << "it might be a very recent output.\n\n";
    } else {
        std::cerr << "Real output confirmed: unlocked=true, height=" << real_check[0].height << "\n";
    }

    const size_t NUM_DECOYS = 15;

    // --- segregation: work out the pool bounds and type split for this source ---
    // Source: wallet2.cpp line 9193-9245 (RCT / amount==0 branch; the
    // use_histogram branches are skipped since this tool only deals with
    // RingCT outputs).
    const bool output_is_pre_fork = is_after_segregation_fork && (real_check[0].height < segregation_fork_height);

    uint64_t num_outs = gp.get_num_rct_outs(); // default: full chain (line 9330: num_outs = gamma->get_num_rct_outs())
    double pre_fork_ratio = 0.0, post_fork_ratio = 0.0;
    bool whole_pool_restricted_to_pre_fork = false;

    if (is_after_segregation_fork && segregate_pre_fork_flag && output_is_pre_fork) {
        // line 9199-9202 looks like it restricts the whole pool to
        // pre-fork outputs, but that only sticks for use_histogram==true
        // (amount != 0). For RCT (amount==0), use_histogram is always
        // false, and wallet2.cpp line 9267-9270 unconditionally resets
        // num_outs back to the full chain a few lines later, in a
        // separate if(use_histogram){...}else{...} block that's
        // independent of the segregation branch above:
        //     else { num_outs = gamma->get_num_rct_outs(); }
        // So for RCT this "restrict the whole pool to pre-fork" branch
        // has no practical effect upstream - segregate_pre_fork_outputs
        // never actually limits the RingCT decoy pool to pre-fork-only
        // outputs, regardless of the flag. This tool matches that: we
        // reset num_outs back to the full chain right away, and keep
        // whole_pool_restricted_to_pre_fork only for reporting in the
        // output JSON (it doesn't affect the draw logic).
        whole_pool_restricted_to_pre_fork = true;
        num_outs = gp.get_num_rct_outs(); // matches wallet2.cpp line 9268 - unconditional reset for RCT
    } else if (is_after_segregation_fork && key_reuse_mitigation2_flag) {
        // line 9219-9243
        if (output_is_pre_fork) {
            pre_fork_ratio = 33.4 / 100.0 * (1.0 - RECENT_OUTPUT_RATIO);   // line 9225/9229
            post_fork_ratio = is_shortly_after_segregation_fork ? 0.0
                             : 33.4 / 100.0 * (1.0 - RECENT_OUTPUT_RATIO); // line 9230
        } else {
            post_fork_ratio = is_shortly_after_segregation_fork ? 0.0
                             : 67.8 / 100.0 * (1.0 - RECENT_OUTPUT_RATIO); // line 9240
        }
    }

    // wallet2 sizes these ratios against a padded candidate pool (75 for
    // RCT with fake_outputs_count=15; see wallet2.cpp line 9161
    // base_requested_outputs_count=25, line 9202
    // +CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW(60)-CRYPTONOTE_DEFAULT_TX_SPENDABLE_AGE(10)=+50
    // => 75), not against NUM_DECOYS(15) directly, and draws the whole
    // pool up front without knowing lock status. It then scans that
    // fixed pool in order (pre-fork, then post-fork, then normal),
    // skipping locked/duplicate entries with no replacement, and stops
    // once NUM_DECOYS(15) valid decoys are found (draw loop bound at
    // line 9370, fill loop stopping condition at ~line 9600).
    const size_t REQUESTED_POOL_SIZE = 75; // wallet2's real value for RCT with fake_outputs_count=15
    size_t pre_fork_draw = 0, post_fork_draw = 0, normal_draw = REQUESTED_POOL_SIZE;
    if (!whole_pool_restricted_to_pre_fork) {
        // Full size of each bucket in the pool (not capped by
        // NUM_DECOYS), truncated the same way wallet2's size_t cast
        // does - no rounding.
        pre_fork_draw  = (size_t)(REQUESTED_POOL_SIZE * pre_fork_ratio);
        post_fork_draw = (size_t)(REQUESTED_POOL_SIZE * post_fork_ratio);
        normal_draw    = REQUESTED_POOL_SIZE - pre_fork_draw - post_fork_draw;
    }

    if (pre_fork_draw || post_fork_draw) {
        std::cerr << "Candidate pool (drawn before lock status is known): "
                  << pre_fork_draw << " pre-fork, " << post_fork_draw << " post-fork, "
                  << normal_draw << " full-chain (total " << REQUESTED_POOL_SIZE << ").\n";
    } else if (whole_pool_restricted_to_pre_fork) {
        std::cerr << "Note: segregate-pre-fork is on and the output predates the fork, but per "
                  << "wallet2's own RCT behavior (line 9268, the unconditional reset of num_outs "
                  << "back to the full chain) this flag has no practical effect on the RCT decoy "
                  << "pool - decoys are still drawn from the full chain (num_outs=" << num_outs
                  << "), matching real wallet2.\n";
    }

    // resolve [lo, hi) bounds by slot type
    auto bounds_for = [&](slot_type t) -> std::pair<uint64_t,uint64_t> {
        if (t == slot_type::PRE_FORK) return {0, seg_limit.till_fork};
        if (t == slot_type::POST_FORK) return {seg_limit.till_fork, num_outs};
        return {0, num_outs}; // NORMAL - num_outs is always the full chain, even under
                                // whole_pool_restricted_to_pre_fork (see the unconditional
                                // reset in wallet2.cpp line 9268)
    };

    // === Phase 1: draw the fixed pool in order (lock status unknown) ===
    // Uniqueness (no index repeated in the pool) and excluding the real
    // output from ever being drawn as a decoy are both enforced here;
    // lock status is checked in Phase 2 with a single batched
    // /get_outs call.
    std::set<uint64_t> seen_in_pool;
    std::vector<std::pair<uint64_t, slot_type>> pool;
    pool.reserve(REQUESTED_POOL_SIZE);

    auto draw_pool_slot = [&](size_t n, slot_type t) {
        auto bounds = bounds_for(t);
        uint64_t lo = bounds.first, hi = bounds.second;
        size_t got = 0;
        size_t local_tries = 0;
        const size_t MAX_LOCAL_TRIES = 400000;
        while (got < n && local_tries < MAX_LOCAL_TRIES) {
            local_tries++;
            uint64_t p;
            try {
                p = bounded_gamma_pick(gp, lo, hi);
            } catch (const std::exception &) { break; } // range exhausted (can happen on small chains)
            if (p == real_index) continue;
            if (seen_in_pool.count(p)) continue;
            seen_in_pool.insert(p);
            pool.push_back({p, t});
            got++;
        }
        if (got < n) {
            std::cerr << "  WARNING: only got " << got << " of the requested " << n
                      << " candidates for the " << (t == slot_type::PRE_FORK ? "pre-fork" : t == slot_type::POST_FORK ? "post-fork" : "normal")
                      << " bucket (the range itself may be small).\n";
        }
    };

    if (whole_pool_restricted_to_pre_fork) {
        // num_outs is the full chain now (see the fix above), so this
        // ends up identical to pre_fork_draw/post_fork_draw=0: all 75
        // candidates get drawn from the NORMAL (full-chain) range -
        // matching real wallet2's RCT behavior, where this flag ends up
        // having no effect.
        draw_pool_slot(REQUESTED_POOL_SIZE, slot_type::NORMAL);
    } else {
        draw_pool_slot(pre_fork_draw, slot_type::PRE_FORK);
        draw_pool_slot(post_fork_draw, slot_type::POST_FORK);
        draw_pool_slot(normal_draw, slot_type::NORMAL);
    }

    // === Phase 2: check lock status for the whole pool in one batch ===
    std::vector<uint64_t> pool_indices;
    pool_indices.reserve(pool.size());
    for (auto &p : pool) pool_indices.push_back(p.first);

    std::vector<out_info> pool_status;
    if (!pool_indices.empty()) {
        pool_status = fetch_outs(rpc_url, pool_indices);
    }

    // === scan the pool in order, skip locked entries with no ===
    // === replacement, stop once NUM_DECOYS(15) are collected ===
    std::vector<uint64_t> confirmed_list;
    size_t accepted_pre = 0, accepted_post = 0, accepted_normal = 0;
    size_t locked_skipped = 0;

    for (size_t i = 0; i < pool.size() && confirmed_list.size() < NUM_DECOYS; ++i) {
        if (!pool_status[i].unlocked) {
            locked_skipped++;
            std::cerr << "  index " << pool[i].first << " is LOCKED (height=" << pool_status[i].height
                      << "), skipping (not replaced - matches real wallet2).\n";
            continue;
        }
        confirmed_list.push_back(pool[i].first);
        if (pool[i].second == slot_type::PRE_FORK) accepted_pre++;
        else if (pool[i].second == slot_type::POST_FORK) accepted_post++;
        else accepted_normal++;
    }

    if (confirmed_list.size() < NUM_DECOYS) {
        std::cerr << "ERROR: the candidate pool (" << pool.size() << " candidates, " << locked_skipped
                  << " locked) ran out with only " << confirmed_list.size() << "/" << NUM_DECOYS
                  << " valid decoys found. Real wallet2 can hit this too under a high enough lock\n"
                  << "rate; we deliberately don't backfill from another bucket, since that's exactly\n"
                  << "what caused the earlier divergence from wallet2's real behavior. Try running\n"
                  << "again (a fresh gamma draw will produce different candidates).\n";
        return 1;
    }

    std::cerr << "Done: selected " << NUM_DECOYS << " decoys from a pool of " << pool.size()
              << " (pre=" << accepted_pre << ", post=" << accepted_post
              << ", normal=" << accepted_normal << ", " << locked_skipped << " skipped as locked).\n\n";

    std::vector<uint64_t> final_list = confirmed_list;
    auto final_details = fetch_outs(rpc_url, final_list);

    struct ring_member { uint64_t index; std::string key; std::string mask; };
    std::vector<ring_member> ring;
    ring.push_back({real_index, real_check[0].key, real_check[0].mask});
    for (size_t i = 0; i < final_list.size(); ++i) {
        ring.push_back({final_list[i], final_details[i].key, final_details[i].mask});
    }

    std::sort(ring.begin(), ring.end(), [](const ring_member &a, const ring_member &b) {
        return a.index < b.index;
    });

    size_t real_output_position = SIZE_MAX;
    for (size_t i = 0; i < ring.size(); ++i) {
        if (ring[i].index == real_index) { real_output_position = i; break; }
    }
    if (real_output_position == SIZE_MAX) {
        throw std::runtime_error("internal error: real index not found in the sorted ring");
    }

    std::vector<uint64_t> key_offsets_relative;
    key_offsets_relative.reserve(ring.size());
    for (size_t i = 0; i < ring.size(); ++i) {
        if (i == 0) key_offsets_relative.push_back(ring[i].index);
        else key_offsets_relative.push_back(ring[i].index - ring[i-1].index);
    }

    std::cerr << "real_output_position = " << real_output_position << ", key_offsets built.\n\n";

    json source_json;
    source_json["amount"] = 0; // RingCT
    source_json["real_output_global_index"] = real_index;
    source_json["real_output_position_in_ring"] = real_output_position;
    source_json["key_offsets_relative"] = key_offsets_relative;
    source_json["segregation"] = {
        {"is_after_segregation_fork", is_after_segregation_fork},
        {"output_is_pre_fork", output_is_pre_fork},
        {"pre_fork_count", accepted_pre},
        {"post_fork_count", accepted_post},
        {"normal_count", accepted_normal},
        {"pool_size", pool.size()},
        {"locked_skipped", locked_skipped},
        {"whole_pool_restricted_to_pre_fork", whole_pool_restricted_to_pre_fork}
    };
    source_json["ring"] = json::array();
    for (auto &m : ring) {
        source_json["ring"].push_back({{"global_index", m.index}, {"public_key", m.key}, {"commitment", m.mask}});
    }
    sources_json.push_back(source_json);
    } // for real_index

    json ring_json;
    // Real fee estimate (get_fee_estimate) - the offline assembler can't
    // reach the network, so we (the online side) fetch it here and put
    // it in ring.json. Fetched once, shared across all sources.
    try {
        json fee_result = rpc_call(rpc_url, "get_fee_estimate", json::object());
        ring_json["fee_per_byte"] = fee_result["fee"];
        ring_json["fee_quantization_mask"] = fee_result.value("quantization_mask", 1);
    } catch (const std::exception &e) {
        std::cerr << "WARNING: could not fetch a fee estimate (" << e.what() << "), assembler will use its default.\n";
    }
    ring_json["sources"] = sources_json;
    ring_json["segregation_fork_height"] = segregation_fork_height;
    ring_json["segregation_active"] = is_after_segregation_fork;
    // The key/mask for ring[real_output_position] in each source come
    // from /get_outs (i.e. the daemon). wallet2 does not trust the
    // daemon for the real output - it recomputes rct::commit(amount,
    // mask) from what it already knows (view key derivation). Your
    // signing layer should do the same (assembler already does).
    ring_json["_warning"] = "for each source's real_output, the key/mask are a /get_outs reference only; "
                             "the signing layer must verify against its own computed value (see wallet2.cpp get_outs())";

    std::ofstream ring_file("ring.json");
    ring_file << ring_json.dump(2);
    ring_file.close();
    std::cerr << "Ring written to ring.json (" << sources_json.size() << " source(s))\n";

    return 0;
}