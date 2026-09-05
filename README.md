# decoy_tool

Selects Monero decoys and builds signable rings from global index data.

> [!NOTE]
> **Note:** The project works for now based on the current RingCT system.
> However, FCMP++ will break it, so I plan to implement fundamental
> changes once it becomes the default on mainnet.

## Status

This has been tested against a live mainnet node and produced correct,
working rings with no errors. It has also gone through several rounds of
independent code review comparing it line by line against
`monero-project/monero`, including compiling the real Monero source
locally and running direct numerical comparisons (not just reading code)
against this tool's output. Every real issue those reviews found has
since been fixed and re-verified the same way: built against the real
source, run, and checked against real numbers. The full list is below
under "Review Notes."

This is a solo-built tool. I'm not claiming a formal security audit, and
I'd still rather have more eyes on it than fewer. If you know this
codebase and want to check my work against `wallet2.cpp` yourself, that's
welcome and genuinely useful. It worked successfully on the mainnet,
but I don't want to give any guarantees without further review.
PLEASE DO NOT USE ON THE MAINNET.

This same note applies across the other tools in this toolchain
(`export_outputs`, `select_transfers`, `assembler`, `lora_bridge_app`).

---

## What this does

`decoy_tool` connects to a Monero node of your choosing and, given only the
**global index** of an output you intend to spend (an output you already
picked yourself, from your own wallet), selects the remaining 15 decoys
using Monero's own gamma-distribution decoy selection algorithm. The
algorithm is ported directly from `monero-project/monero`
(`src/wallet/wallet2.cpp`, the `gamma_picker` class and
`wallet2::get_outs()`). It fetches on-chain data for every ring member
from the node and writes a `ring.json` that `assembler` can consume to
sign a transaction.

The point of doing it this way: get decoys with the same statistical
properties the official wallet produces, without needing a full watch-only
wallet synced to the network. Picking which output to spend, exporting
wallet data, and assembling/signing the transaction are handled by the
other tools in this toolchain.

## Segregation (pre-fork / post-fork decoy pool split)

`wallet2::get_outs()` doesn't always draw every RingCT decoy from a single
pool. When `m_segregate_pre_fork_outputs` and/or `m_key_reuse_mitigation2`
are enabled (both default to `true` in `wallet2`), it can split the decoy
pool between outputs created before and after a configured fork height.
This was a historical mitigation against a key-reuse privacy attack. This
tool implements that same split, matching `wallet2.cpp`'s `get_outs()`
RCT branch as closely as a standalone tool reasonably can.

Source-verified detail: `SEGREGATION_FORK_HEIGHT`,
`TESTNET_SEGREGATION_FORK_HEIGHT`, and `STAGENET_SEGREGATION_FORK_HEIGHT`
are all defined as `99999999` in upstream Monero, a sentinel for a fork
that was anticipated but never happened. A stock `wallet2` only uses a
different value if the user explicitly passes `segregation-height <n>` in
`simplewallet` or `monero-wallet-rpc`. Real chain heights are nowhere near
that sentinel, so a default `wallet2` instance never actually enters this
code path on mainnet, testnet, or stagenet. This tool mirrors that
exactly:

- By default (no `--segregation-height` given), the sentinel is used, the
  segregation branch never activates, and decoy selection behaves the
  same way it does for virtually every real Monero wallet in use today
  (single gamma pool, no split).
- If you pass `--segregation-height=N` (to test the mechanism, or because
  you're deliberately preparing for a specific fork scenario), the tool
  reproduces `wallet2`'s actual two-phase process: it draws a fixed
  75-candidate pool up front, in a fixed order (pre-fork slice first,
  then post-fork, then the rest of the chain), sized by the same
  33.4% / 67.8% ratios (scaled by `1 - RECENT_OUTPUT_RATIO`) `wallet2`
  uses. It then checks the unlock status of that whole pool in one batch
  and scans it in that same fixed order, skipping any locked candidate
  without replacing it from the same bucket, stopping once 15 valid
  decoys are found. This matters: if a bucket has more locked candidates
  than usual, `wallet2`'s actual ring composition shifts toward later
  buckets rather than holding a fixed ratio, and this tool now does the
  same. It also replicates a specific, easy-to-miss quirk in `wallet2`
  itself: for RingCT outputs, the "restrict the whole pool to pre-fork
  only" branch gets its effect silently cancelled a few lines later by
  an unconditional reset of the candidate count to the full chain, so in
  practice that particular restriction never actually applies to RingCT
  decoys in real `wallet2` either. This tool now matches that, rather
  than enforcing a restriction upstream itself doesn't enforce.
- `--no-segregate-pre-fork` and `--no-key-reuse-mitigation2` mirror the
  corresponding `wallet2` config flags (`segregate_pre_fork_outputs()`,
  `key_reuse_mitigation2()`), both on by default, matching `wallet2`'s own
  constructor defaults.

If the fixed candidate pool runs out before 15 valid decoys are found
(possible if lock rates are unusually high), the tool fails loudly with
a clear error rather than silently drawing more from whatever bucket is
short. That's deliberate: quietly topping up a depleted bucket is exactly
the kind of shortcut that made an earlier version of this logic diverge
from `wallet2`'s real behavior under load. Failing and letting you rerun
it is the safer choice.

## Review Notes

These are things independent review found across several rounds. Most
are fixed now; I'm keeping the record here rather than deleting it,
since knowing what was wrong and got caught is more useful than a clean
slate would be.

- **Handled:** decoy "locked or not" status is trusted from the connected
  node's `/get_outs` response (its `unlocked` field), with no client-side
  re-derivation of unlock height. This is also how `wallet2` itself
  behaves, so it isn't a new weakness introduced by this tool. Worth
  being clear about what it means though: a malicious or misconfigured
  node could in principle bias which decoys are offered as candidates by
  misreporting lock status. It doesn't change the math of the gamma
  distribution or the segregation ratios, and it's a trust assumption
  you're already making by picking which node to connect to. Use a node
  you trust, the same advice that applies to any Monero wallet.

- **Fixed: the segregation draw and unlock-check phases were merged in a
  way that hid a real divergence from `wallet2`.** An earlier version
  retried a specific bucket (pre-fork, post-fork, or the rest of the
  chain) until it had confirmed enough unlocked candidates from that
  exact bucket, no matter how many attempts it took. That always
  produced the same target ratio regardless of how many candidates
  turned out to be locked. Real `wallet2` doesn't do this: it draws a
  fixed pool once, in a fixed order, and if a bucket runs short it just
  moves on to the next one in that order rather than backfilling. Under
  normal conditions (few or no locked candidates) both approaches land
  on the same ring composition, which is why this took two review passes
  to catch. Under higher lock rates, they diverge, and the divergence is
  a real, verifiable difference in ring composition, not a rounding
  error. This is fixed now, verified by running the tool's actual draw
  and fill logic against varying simulated lock rates and confirming the
  ratio shifts the way `wallet2`'s does.

- **Fixed: RingCT decoys under "whole pool restricted to pre-fork" weren't
  matching `wallet2`'s actual (quirky) behavior.** As described above,
  `wallet2` sets the RCT candidate pool to the full chain unconditionally,
  a few lines after code that looks like it restricts the pool to
  pre-fork-only outputs. That earlier restriction never survives to
  matter for RCT. This tool used to actually apply the restriction, which
  meant that in this one specific case, it was more restrictive than real
  `wallet2` rather than matching it. Now it doesn't apply the restriction
  either, matching upstream's real behavior rather than its apparent one.

If you spot something else, please open an issue or a PR. I'd rather be
corrected in public now than have someone run into it the hard way later.

## Dependencies
**Arch Linux**
```bash
sudo pacman -S --needed base-devel curl
sudo pacman -S nlohmann-json
```
### Other Linux Distributions
**Ubuntu/Debian:**
```bash
sudo apt-get install build-essential curl nlohmann-json3-dev
```
**Fedora:**
```bash
sudo dnf install @development-tools curl nlohmann-json-devel
```
**openSUSE:**
```bash
sudo zypper install -t pattern devel_basis curl nlohmann-json-devel
```
## Building
```bash
cd decoy_tool
make
```
This takes a few seconds. There's no real barrier to just trying it.

## Usage
```bash
./decoy_tool http://YOUR_RPC:PORT REAL_GLOBAL_INDEX [INDEX2 ...] \
    [--segregation-height=N] [--no-segregate-pre-fork] [--no-key-reuse-mitigation2]
```
Example (default behavior, segregation off, matches stock wallet2):
```bash
./decoy_tool http://127.0.0.1:18081 1234567
```
Example (multiple sources):
```bash
./decoy_tool http://127.0.0.1:18081 1234567 2345678
```
Example (testing segregation against a specific fork height):
```bash
./decoy_tool http://127.0.0.1:18081 1234567 --segregation-height=1500000
```
- You can write multiple global indexes by simply adding a space between
  them and typing the next one.

> [!NOTE]
> At the end of the process, `ring.json` contains the ring or
> rings in a format the assembler can understand, along with fees pulled
> from the network. You'll use this file directly with the assembler tool.
> Each source also includes a `segregation` block in `ring.json` showing
> whether segregation was active for that pick, and if so, exactly how
> many of the 15 decoys came from the pre-fork pool, the post-fork pool,
> and the full chain, plus how many candidates were skipped for being
> locked. This makes the split verifiable just by reading the output
> file, without re-running the tool.

## If you have five minutes

Clone it, build it (seconds), point it at a node, and see what it does.
If you know the Monero codebase, even better: pull up `wallet2.cpp` next
to `main.cpp` and `gamma_picker.cpp` and tell me what you find. That kind
of feedback is always welcome.
