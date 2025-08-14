# Hako CLI

A command-line interface for executing JavaScript through the Hako engine, a lightweight WebAssembly-based JavaScript runtime.

## Overview

This CLI provides direct access to Hako for running JavaScript files from the command line. Built on WAMR (WebAssembly Micro Runtime), it delivers a single executable with no external dependencies while maintaining the security benefits of WebAssembly's sandboxed execution.

## Building

Requires CMake 3.14+ and a C23-compatible compiler.

```bash
git clone --recursive https://github.com/andrewmd5/hako-cli
cd hako-cli
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

The executable will be available at `build/bin/hako`.

## Usage

### Evaluate JavaScript
```bash
# Run a file
hako script.js

# Inline evaluation
hako -e "console.log(Math.PI * 2)"

# From stdin
echo "console.log('hello')" | hako
```

### Compile to Bytecode
```bash
# Compile to .jco bytecode
hako -c script.js

# Run compiled bytecode (faster startup)
hako script.jco
```

Bytecode files skip parsing and compilation, useful for production or frequently-run scripts.

### Examples

Parse logs:
```bash
tail -f ~/test.log | hako -e "while(line = readline()) {
  const match = line.match(/status:(\d+) bytes:(\d+)/);
  if (match) console.log('Status:', match[1], 'Size:', match[2]);
}"
```

Process JSON:
```bash
curl -s https://api.github.com/repos/andrewmd5/hako | \
  hako -e "const data = JSON.parse(read()); data.stargazers_count"
```

## License

See the main Hako repository at https://github.com/andrewmd5/hako for licensing information.