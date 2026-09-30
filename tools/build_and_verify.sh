#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-gcc}
DEFAULT_CFLAGS="-O3 -std=c11 -march=native -pthread -I$ROOT/include -I$ROOT/src"
CFLAGS="${CFLAGS:-$DEFAULT_CFLAGS}"
mkdir -p "$ROOT/build"

$CC $CFLAGS "$ROOT/src/UltraFastRng512_MC_NORMALTAIL_OPTIMIZED_V1.c" -lcrypto -lm -o "$ROOT/build/ufr_v1"
$CC $CFLAGS "$ROOT/bench/bench_speed_showcase.c" -lcrypto -lm -o "$ROOT/build/bench_speed_v1"
$CC $CFLAGS "$ROOT/bench/bench_speed_fresh_sample.c" -lcrypto -lm -o "$ROOT/build/bench_speed_fresh_sample_v1"
$CC $CFLAGS "$ROOT/bench/bench_options.c" -lcrypto -lm -o "$ROOT/build/bench_options_v1"
$CC $CFLAGS "$ROOT/bench/bench_quality_mc_input.c" -lcrypto -lm -o "$ROOT/build/bench_quality_mc_input_v1"
$CC $CFLAGS "$ROOT/bench/bench_quality_options.c" -lcrypto -lm -o "$ROOT/build/bench_quality_options_v1"
$CC $CFLAGS "$ROOT/bench/bench_quality_anti_reuse_deep.c" -lcrypto -lm -o "$ROOT/build/bench_quality_anti_reuse_deep_v1"

"$ROOT/build/ufr_v1" verify
printf 'BUILD_AND_VERIFY: PASS\n'
printf 'V1 executables:\n'
printf '  build/ufr_v1 verify\n'
printf '  build/bench_speed_v1 [chunks=16384 runs=3 generation_chunks=1048576]\n'
printf '  build/bench_speed_fresh_sample_v1 [chunks=4096 runs=3]\n'
printf '  build/bench_options_v1 [chunks=1024 generation=1048576 runs=3]\n'
printf '  build/bench_quality_mc_input_v1 [groups=8]\n'
printf '  build/bench_quality_options_v1 [256 q256 blocks]\n'
printf '  build/bench_quality_anti_reuse_deep_v1\n'
