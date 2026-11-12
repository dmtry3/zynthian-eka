# Sourced by build.sh, test.sh, bench.sh: compiles the C++ engines (src/plaits/) into $1 with the flags $2
# and sets ENGINE_OBJS. g++ without exceptions, RTTI and the STL (TZ 7); the rest of the plugin stays C.
build_engines() {
    local out="$1" flags="$2"
    mkdir -p "$out"
    ENGINE_OBJS=""
    for f in engines tables; do
        g++ $flags -std=c++11 -fno-exceptions -fno-rtti -Wall -Wextra -c "$ROOT/src/plaits/$f.cc" -o "$out/$f.o"
        ENGINE_OBJS="$ENGINE_OBJS $out/$f.o"
    done
}
