set -e
A=../mesa-src/src/amd/addrlib
mkdir -p obj
for f in $A/src/addrinterface.cpp $A/src/core/*.cpp $A/src/gfx9/*.cpp $A/src/gfx10/*.cpp $A/src/gfx11/*.cpp $A/src/gfx12/*.cpp $A/src/r800/*.cpp; do
  o=obj/$(basename $f .cpp).o
  [ -f $o ] || nice -n 19 g++ -O1 -std=c++17 -w -DADDR_FASTCALL= -DLITTLEENDIAN_CPU -DDEBUG=0 -I$A/inc -I$A/src -I$A/src/core -I$A/src/chip/gfx9 -I$A/src/chip/r800 -I$A/src/chip/gfx10 -I$A/src/chip/gfx11 -I$A/src/chip/gfx12 -c $f -o $o &
done
wait
ar rcs libaddr.a obj/*.o
