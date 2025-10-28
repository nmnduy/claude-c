# Release Process Guide

This document explains how automated releases work for Claude C - Pure C Edition.

## Automated Release Workflow

### Triggering Releases

There are two ways to trigger automated releases:

#### 1. Tag-based Releases (Recommended)
```bash
# Create a new version tag and push
git tag v1.0.0
git push origin v1.0.0

# For pre-releases
git tag v1.0.0-beta.1
git push origin v1.0.0-beta.1
```

#### 2. Manual Workflow Dispatch
1. Go to your repository's **Actions** tab
2. Select **Build and Release** workflow
3. Click **Run workflow**
4. Fill in the version and prerelease settings

### What Happens Automatically

The GitHub Actions workflow will:

1. **Build binaries for multiple platforms:**
   - Linux x86_64 (statically linked)
   - macOS x86_64 (dynamically linked with Homebrew deps)
   - Windows x86_64 (MSVC build with vcpkg)

2. **Create release archives:**
   - `claude-c-linux-x86_64.tar.gz`
   - `claude-c-macos-x86_64.tar.gz`
   - `claude-c-windows-x86_64.zip`

3. **Generate GitHub Release:**
   - Automatic release notes from recent commits
   - All binaries uploaded as release assets
   - Prerelease flag automatically detected from version

4. **Build and publish Docker image:**
   - Multi-platform (linux/amd64, linux/arm64)
   - Tags: `latest`, `v1.0.0`, `v1.0`, `v1`
   - Hosted on GitHub Container Registry

## Version Numbering

Use [Semantic Versioning](https://semver.org/):

- **Major releases:** `v1.0.0` (breaking changes)
- **Minor releases:** `v1.1.0` (new features, backward compatible)
- **Patch releases:** `v1.0.1` (bug fixes)
- **Pre-releases:** `v1.0.0-alpha.1`, `v1.0.0-beta.2`, `v1.0.0-rc.1`

## Release Assets

### Binary Downloads

Each release includes:

- **Linux:** Static binary, no dependencies required
- **macOS:** Binary requiring Homebrew packages (`curl cjson sqlite3`)
- **Windows:** Self-contained executable with all dependencies

### Docker Images

Available from GitHub Container Registry:

```bash
# Pull the latest version
docker pull ghcr.io/yourusername/claude-c:latest

# Pull a specific version
docker pull ghcr.io/yourusername/claude-c:v1.0.0

# Run the container
docker run --rm -it \
  -e OPENAI_API_KEY="$OPENAI_API_KEY" \
  -v $(pwd):/workspace \
  ghcr.io/yourusername/claude-c:latest "your prompt here"
```

## Installation Instructions

### Binary Installation

#### Linux
```bash
# Download and extract
wget https://github.com/yourusername/claude-c/releases/download/v1.0.0/claude-c-linux-x86_64.tar.gz
tar -xzf claude-c-linux-x86_64.tar.gz
cd claude-c-linux-x86_64

# Install binary
sudo cp claude-c /usr/local/bin/
chmod +x /usr/local/bin/claude-c

# Or install to user directory
mkdir -p ~/.local/bin
cp claude-c ~/.local/bin/
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
```

#### macOS
```bash
# Install dependencies first
brew install curl cjson sqlite3

# Download and extract
curl -L -o claude-c-macos-x86_64.tar.gz \
  https://github.com/yourusername/claude-c/releases/download/v1.0.0/claude-c-macos-x86_64.tar.gz
tar -xzf claude-c-macos-x86_64.tar.gz
cd claude-c-macos-x86_64

# Install
sudo cp claude-c /usr/local/bin/
chmod +x /usr/local/bin/claude-c
```

#### Windows
```powershell
# Download and extract
Invoke-WebRequest -Uri "https://github.com/yourusername/claude-c/releases/download/v1.0.0/claude-c-windows-x86_64.zip" -OutFile "claude-c.zip"
Expand-Archive -Path claude-c.zip -DestinationPath .
cd claude-c-windows-x86_64

# Add to PATH or copy to desired location
```

### Docker Installation
```bash
# Pull and run
docker run --rm -it \
  -e OPENAI_API_KEY="$OPENAI_API_KEY" \
  -v $(pwd):/workspace \
  ghcr.io/yourusername/claude-c:latest
```

### Package Manager Installation (Future)

Planned support for:
- Homebrew (macOS)
- APT (Debian/Ubuntu)
- YUM/DNF (Fedora/RHEL)
- Scoop (Windows)

## Release Checklist

Before creating a release:

### Pre-release Checklist
- [ ] All tests pass: `make test`
- [ ] Memory tests pass: `make memscan`
- [ ] Documentation updated (README.md, CHANGELOG.md)
- [ ] Version number updated in source (if needed)
- [ ] CHANGELOG.md updated with changes since last release
- [ ] License file present
- [ ] No sensitive information in binaries

### Post-release Checklist
- [ ] Release created successfully
- [ ] All binaries uploaded
- [ ] Docker image built and pushed
- [ ] Release notes are accurate
- [ ] Test download and installation on each platform
- [ ] Update website/documentation with new version

## Troubleshooting

### Build Failures

#### Windows Build Issues
- Ensure vcpkg dependencies are properly installed
- Check CMake configuration for MSVC compatibility
- Verify Visual Studio Build Tools are available

#### macOS Build Issues
- Ensure Xcode Command Line Tools are installed
- Check Homebrew packages are up to date
- Verify code signing requirements (if any)

#### Linux Build Issues
- Check static linking flags
- Verify all dependencies are available
- Test on multiple distributions if possible

### Release Issues

#### Missing Assets
- Check GitHub Actions logs for build failures
- Verify artifact upload steps completed
- Check file paths and naming conventions

#### Version Conflicts
- Ensure tags are properly formatted (vX.Y.Z)
- Check for existing tags with same version
- Verify semantic versioning compliance

## Manual Release Process

If automated releases fail, you can create releases manually:

### 1. Build Binaries Locally
```bash
# Linux
make clean
make CFLAGS="-O3 -DNDEBUG -static"

# macOS
make clean
make

# Windows (in MSVC environment)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### 2. Create Archives
```bash
# Linux
mkdir claude-c-linux-x86_64
cp build/claude-c claude-c-linux-x86_64/
cp README.md LICENSE claude-c-linux-x86_64/
tar -czf claude-c-linux-x86_64.tar.gz claude-c-linux-x86_64/

# Similar for other platforms
```

### 3. Create GitHub Release
1. Go to **Releases** page
2. Click **Create a new release**
3. Enter tag and title
4. Upload archives
5. Write release notes
6. Publish release

## Security Considerations

### Binary Security
- Binaries are built on GitHub's infrastructure
- Source code is publicly available for audit
- No proprietary binaries included
- Dependencies are from trusted sources

### Supply Chain Security
- GitHub Actions uses pinned action versions
- Dependencies are from official repositories
- Docker images use minimal base images
- No root privileges in Docker containers

### API Security
- API keys are not included in binaries
- Configuration via environment variables only
- No hardcoded credentials
- Secure communication with APIs

## Contributing to Releases

### Testing Pre-releases
- Download and test pre-release binaries
- Report issues on GitHub Issues
- Test on your specific platform/configuration
- Provide feedback on installation process

### Release Process Improvements
- Suggest improvements to the workflow
- Add support for new platforms
- Improve documentation
- Add automated tests

## Future Enhancements

### Planned Improvements
- [ ] Automatic package manager publishing (Homebrew, APT, etc.)
- [ ] Code signing for binaries
- [ ] Automated security scanning
- [ ] Performance benchmarking
- [ ] More architecture support (ARM64, etc.)

### Platform Support
- [ ] FreeBSD binaries
- [ ] ARM64 binaries for all platforms
- [ ] musl-based Alpine Linux binaries
- [ ] Embedded Linux variants

### Distribution
- [ ] Snap packages
- [ ] Flatpak packages
- [ ] Chocolatey packages (Windows)
- [ ] Winget packages (Windows)