![mpv logo](https://raw.githubusercontent.com/mpv-player/mpv.io/master/source/images/mpv-logo-128.png)

## Building dmpv

### Quick Start
```bash
./configure
make -j$(nproc)
```

### Build Performance Optimization

The bliss build system supports several optimizations for faster compilation:

#### 1. Parallel Builds (Recommended)
Always use parallel builds to utilize all CPU cores:
```bash
make -j$(nproc)
```

#### 2. Compiler Cache (ccache)
Use ccache to dramatically speed up rebuilds:
```bash
CC="ccache gcc" ./configure
make -j$(nproc)
```

Install ccache: `sudo apt install ccache` or `sudo dnf install ccache`

#### 3. Disable Debug Symbols
For faster compilation (especially during development iterations):
```bash
./configure --disable-debug-build
make -j$(nproc)
```

This reduces compilation time significantly by omitting debug symbols (no `-g3` flag).

#### 4. Combined Optimizations
For maximum build speed:
```bash
CC="ccache gcc" ./configure --disable-debug-build
make -j$(nproc)
```

### Build System Improvements

Recent optimizations to the bliss build system:
- Removed redundant `mkdir -p` calls (one per file → one per directory)
- Added order-only prerequisites for directory creation
- Enabled `--output-sync` for cleaner parallel build output
- Made debug symbols optional to reduce compilation overhead
- Optimized Wayland protocol generation dependencies
