#!/bin/bash
# bash script to install the latest version of cmake and other packages required by the ladybird.sh script

sleepandclear() {
sleep 0.5
clear
}
if [ -d /etc/apt ]; then
if grep -q 'sudo apt update' ~/.bash_history; then
   echo "packages already updated, not updating..."
else
   echo "system is not updated, updating..."
   sudo apt update
fi
   sleepandclear
   echo "installing required packages..."
   echo "checking if wget is installed..."
   if [ -f /usr/bin/wget ]; then
      echo "wget is installed, not installing."
   else
      echo "wget is not installed installing wget..."
      sudo apt install wget -y
      echo "wget is installed."
    fi
      sleepandclear
      echo "checking if tar is installed..."
    if [ -f /usr/bin/tar ]; then
      echo "tar installed not installing."
    else
      echo "tar is not installed, installing..."
      sudo apt install tar -y
      sleepandclear
      echo "tar is installed"
    fi
      sleepandclear
      echo "checking if pkg-config installed on your system..."
    if [ -f /usr/bin/pkg-config ]; then
       echo "pkg-config installed, not installling."
    else
       echo "pkg-config is not installed, installing..."
       sudo apt install -y pkg-config
       echo "pkg-config is installed."
    fi
    echo "checking if addr2line is installed..."
    if [ -f /usr/bin/addr2line ]; then
       echo "addr2line installed, not installing."
    else
       echo "addr2line is not installed, installing..."
       sudo apt install binutils -y
       echo "addr2line is installed."
    fi
      sleepandclear
      if ! command -v cmake; then
      sudo apt install cmake 1>&2 > /dev/null
      else
      echo "cmake installed, not installing."
      fi
      cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt 
      sleepandclear
      if grep 'cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt ' ~/.bash_history; then
         echo "command 'grep -oP '\d+\.\d+\.\d+' cmake_version.txt' already executed, not executing again."
      else
         echo "cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt ' not executed, executing..."
         grep -oP '\d+\.\d+\.\d+' cmake_version.txt 
      fi
      check_cmake_version=$(cat -v cmake_version.txt)
      if [ ! "$check_cmake_version" = "3.25"  ]; then
      echo "incorrect version of cmake installed, installing the required cmake version. '3.25+' "
      rm -rf cmake_version.txt
      sleepandclear
      else
      echo "incorrect version of cmake, installing the required cmake version. '3.25+' "
      sleepandclear
      echo "removing old cmake version..."
      sudo remove --purge cmake -y
      sleepandclear
      echo "downloading tarfile..."
      wget https://github.com/Kitware/CMake/releases/download/v4.0.1/cmake-4.0.1-linux-x86_64.tar.gz
      sleepandclear 
      echo "done, downloading"
      sleepandclear
      echo "extracting tarfile..."
      tar -xzf cmake-4.0.1-linux-x86_64.tar.gz
      echo "done lsextracting tarfile, changing current path to extracted tar directory..."
      cd cmake-4.0.1-linux-x86_64
      sudo mv bin/* /usr/bin/ 
      sudo mv -f share/* /usr/share/ 
      cd ..
      echo "done moving files to /usr/bin and /usr/share, removing tarfile..."
      rm -rf cmake-4.0.1-linux-x86_64
      echo "done installing cmake."
      rm cmake-4.0.1-linux-x86_64.tar.g*
      rm cmake_version.txt
    fi
fi
if [ -d /etc/dnf ]; then
if grep -q 'sudo dnf update' ~/.bash_history; then
   echo "packages already updated, not updating..."
else
   echo "system is not updated, updating..."
   sudo dnf update
fi
   sleepandclear
   echo "checking if wget is installed..."
   if [ -f /usr/bin/wget ]; then
      echo "wget is installed, not installing."
   else
      echo "wget is not installed installing wget..."
      sudo dnf install wget -y
      echo "wget is installed."
    fi
      sleepandclear
      echo "checking if tar is installed..."
    if [ -f /usr/bin/tar ]; then
      echo "tar is installed not installing."
    else
      echo "tar is not installed, installing..."
      sudo dnf install tar -y
      sleepandclear
      echo "tar is installed"
    fi
      sleepandclear
      echo "installing required packages..."
      cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt 
      sleepandclear
      if grep 'cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt ' ~/.bash_history; then
         echo "command 'grep -oP '\d+\.\d+\.\d+' cmake_version.txt' already executed, not executing again."
      else
         echo "cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt ' not executed, executing..."
         grep -oP '\d+\.\d+\.\d+' cmake_version.txt 
      fi
      check_cmake_version=$(cat -v ./cmake_version.txt)
      if [ ! "$check_cmake_version" = "3.25"  ]; then
      echo "incorrect version of cmake installed, installing the required cmake version. '3.25+' "
      rm -rf cmake_version.txt
      sleepandclear
   else
      echo "incorrect version of cmake, installing the required cmake version. '3.25+' "
      sleepandclear
      echo "removing old cmake version..."
      sudo dnf remove cmake -y
      sleepandclear
      echo "downloading tarfile..."
      wget https://github.com/Kitware/CMake/releases/download/v4.0.1/cmake-4.0.1-linux-x86_64.tar.gz
      sleepandclear 
      echo "done, downloading"
      sleepandclear
      echo "extracting tarfile..."
      tar -xzf cmake-4.0.1-linux-x86_64.tar.gz
      echo "done lsextracting tarfile, changing current path to extracted tar directory..."
      cd cmake-4.0.1-linux-x86_64
      sudo mv bin/* /usr/bin/ 
      sudo mv share/* /usr/share/ 
      cd ..
      echo "done moving files to /usr/bin and /usr/share, removing tarfile..."
      rm -rf cmake-4.0.1-linux-x86_64
      echo "done installing cmake."
      rm cmake-4.0.1-linux-x86_64.tar.g*
      rm cmake_version.txt
    fi
fi
if [ -d /etc/pacman.d ]; then
if grep -q 'sudo pacman -Syu | sudo  yay -Syu | sudo paru -Syu' ~/.bash_history; then
   echo "packages already updated, not updating..."
else
   echo "system is not updated, updating..."
   sudo pacman -Syu update
fi
   sleepandclear
   echo "installing required packages..."
   sleepandclear
   echo "checking if wget is installed..."
   if [ -f /usr/bin/wget ]; then
      echo "wget is installed, not installing."
   else
      echo "wget is not installed installing wget..."
      sudo pacman -S install wget 
      echo "wget is installed."
    fi
      sleepandclear
      echo "checking if tar is installed..."
    if [ -f /usr/bin/tar ]; then
      echo "tar is installed not installing."
    else
      echo "tar is not installed, installing..."
      sudo pacman -S tar 
      sleepandclear
      echo "tar is installed"
    fi
      echo "checking if pkg-config installed on your system..."
    if [ -f /usr/bin/pkg-config ]; then
       echo "pkg-config installed, not installling."
    else
       echo "pkg-config is not installed, installing..."
       sudo pacman -S pkg-config
       echo "pkg-config is installed."
    fi
    echo "checking if addr2line is installed..."
    if [ -f /usr/bin/addr2line ]; then
       echo "addr2line is installed, not installing."
    else
       echo "addr2line is not installed, installing..."
       sudo pacman S binutils 
       echo "addr2line is installed."
    fi
      sleepandclear
      echo "installing required packages..."
      cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt 
      sleepandclear
      if grep 'cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt ' ~/.bash_history; then
         echo "command 'grep -oP '\d+\.\d+\.\d+' cmake_version.txt' already executed, not executing again."
      else
         echo "cmake --version | grep -oP '\d+\.\d+\.\d+' >> cmake_version.txt ' not executed, executing..."
         grep -oP '\d+\.\d+\.\d+' cmake_version.txt 
      fi
      check_cmake_version=$(cat -v ./cmake_version.txt)
      if [ ! "$check_cmake_version" = "3.25"  ]; then
      echo "incorrect version of cmake installed, installing the required cmake version. '3.25+' "
      rm -rf cmake_version.txt
      sleepandclear
   else
      echo "incorrect version of cmake, installing the required cmake version. '3.25+' "
      sleepandclear
      echo "removing old cmake version..."
      sudo pacman -Rsc cmake 
      sleepandclear
      echo "downloading tarfile..."
      wget https://github.com/Kitware/CMake/releases/download/v4.0.1/cmake-4.0.1-linux-x86_64.tar.gz
      sleepandclear 
      echo "done, downloading"
      sleepandclear
      echo "extracting tarfile..."
      tar -xzf cmake-4.0.1-linux-x86_64.tar.gz
      echo "done lsextracting tarfile, changing current path to extracted tar directory..."
      cd cmake-4.0.1-linux-x86_64
      sudo mv bin/* /usr/bin/ 
      sudo mv share/* /usr/share/ 
      cd ..
      echo "done moving files to /usr/bin and /usr/share, removing tarfile..."
      rm -rf cmake-4.0.1-linux-x86_64
      echo "done installing cmake."
      rm cmake-4.0.1-linux-x86_64.tar.g*
      rm cmake_version.txt
    fi
fi
