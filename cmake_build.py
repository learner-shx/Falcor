import subprocess
import sys
import os
# 运行CMake配置命令
# subprocess.check_call(["cmake -B build"])
os.system(r'cmake -B build -DCMAKE_CUDA_COMPILER="C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.1\bin\nvcc.exe"')

os.system(r"cd build/bin/Debug")
# os.system(r"setpath.bat")
# os.system(r"setpath.ps1")
