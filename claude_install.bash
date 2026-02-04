    1  apt install make
    2  apt install gmake
    3  apt update
    4  apt install make
    5  apt install gcc
    6  apt install g++
    7  apt install nasm
    8  apt install gmake
    9  apt install libass
   10  cd libass-0.17.4/
   11  ls
   12  apt install libass-dev
   13  apt search libfdk_aac
   14  apt search libklvanc
   15  apt-get update -qq && apt-get -y install   autoconf   automake   build-essential   cmake   git-core   libass-dev   libfreetype6-dev   libgnutls28-dev   libmp3lame-dev   libsdl2-dev   libtool   libva-dev   libvdpau-dev   libvorbis-dev   libxcb1-dev   libxcb-shm0-dev   libxcb-xfixes0-dev   meson   ninja-build   pkg-config   texinfo   wget   yasm   zlib1g-dev
   16  apt search libfdk_aac
   17  apt search libfdk_aac-dev
   18  apt search libfdk
   19  apt search libfdk-aac2
   20  apt install libfdk-aac-dev libfdk-aac2
   21  apt search libklvanc
   22  apt install libklvanc
   23  cd ..
   24  ls
   25  cd build/libklvanc/
   26  make install
   27  ldconfig
   28  apt install gawk
   29  make install
   30  ldconfig
   31  apt install opus
   32  apt install libopus-dev
   33  apt install libvpx-dev
   34  apt install libx264
   35  apt install libx264-dev
   36  apt install libx265-dev
   37  apt install libx265
   38  apt-get install libx265-dev libnuma-dev
   39  exit
   40  history
   41  history > claude_install.bash
