pkg update
pkg install git wget
mkdir -p ~/.local/bin
[ ! -f ~/.bashrc ] && cp $PREFIX/etc/bash.bashrc ~/.bashrc
local="~/.local/bin"
if [ -d "$local" ] && [[ ":$PATH:" != *":$local:"* ]]; then
    PATH="${PATH:+"$PATH:"}$local"
fi
git clone https://github.com/termux/termux-packages.git
mkdir -p ./termux-packages/x11-repo/gw
cp ./gw/deps/build.sh ./termux-packages/x11-repo/gw
./termux-packages/scripts/./setup-termux.sh
./termux-packages/build-package.sh -I ~/termux-packages/x11-repo/gw
rm -rf termux-packages
source ~/.bashrc
