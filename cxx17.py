# cxx17.py — force C++17 for the C++ files only (ymfm needs >= C++14), leaving the C files (.c)
# on their own -std. Referenced by [env:gosowav] extra_scripts.
Import("env")
env.Append(CXXFLAGS=["-std=gnu++17"])
