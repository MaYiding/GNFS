#!/usr/bin/env bash

# Install the release-only GCC 12 toolchain inside the pinned Ubuntu 20.04
# container. The caller owns the minimal curl/GnuPG bootstrap and the exact
# repository checkout; this script owns the authenticated PPA boundary.

set -euo pipefail

if ((EUID != 0)); then
    echo 'The Linux release toolchain installer must run as root.' >&2
    exit 1
fi

if [[ "$(dpkg --print-architecture)" != amd64 ]]; then
    echo "Unsupported release architecture: $(dpkg --print-architecture)" >&2
    exit 1
fi

if [[ "$(getconf GNU_LIBC_VERSION)" != 'glibc 2.31' ]]; then
    echo "Unsupported release libc: $(getconf GNU_LIBC_VERSION)" >&2
    exit 1
fi

apt_opts=(
    -o Acquire::Retries=4
    -o Acquire::http::Timeout=30
    -o Acquire::https::Timeout=30
)
key_fingerprint='C8EC952E2A0E1FBDC5090F6A2C277A0A352154E5'
key_url="https://keyserver.ubuntu.com/pks/lookup?op=get&options=mr&exact=on&search=0x${key_fingerprint}"
key_file=$(mktemp)
gpg_home=$(mktemp -d)

cleanup() {
    rm -f -- "${key_file}"
    rm -rf -- "${gpg_home}"
}
trap cleanup EXIT

chmod 0700 "${gpg_home}"
export GNUPGHOME="${gpg_home}"

for attempt in 1 2 3 4; do
    if curl --proto '=https' --proto-redir '=https' --fail --silent \
        --show-error --location --connect-timeout 15 --max-time 60 \
        --output "${key_file}" "${key_url}"; then
        break
    fi
    if [[ "${attempt}" -eq 4 ]]; then
        echo 'Unable to fetch the pinned Ubuntu Toolchain PPA key.' >&2
        exit 1
    fi
    sleep $((attempt * 2))
done

primary_fingerprints=$(
    gpg --batch --no-options --homedir "${gpg_home}" \
        --show-keys --with-colons "${key_file}" |
        awk -F: '$1 == "pub" { want = 1; next } want && $1 == "fpr" { print $10; want = 0 }'
)
if [[ "${primary_fingerprints}" != "${key_fingerprint}" ]]; then
    echo 'Ubuntu Toolchain PPA key fingerprint mismatch.' >&2
    exit 1
fi

install -d -m 0755 /etc/apt/keyrings
gpg --batch --no-options --homedir "${gpg_home}" --yes --dearmor \
    --output /etc/apt/keyrings/ubuntu-toolchain-r-test.gpg "${key_file}"
chmod 0644 /etc/apt/keyrings/ubuntu-toolchain-r-test.gpg
cat > /etc/apt/sources.list.d/ubuntu-toolchain-r-test.sources <<'EOF'
Types: deb
URIs: https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu/
Suites: focal
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/ubuntu-toolchain-r-test.gpg
EOF

apt-get "${apt_opts[@]}" update
DEBIAN_FRONTEND=noninteractive apt-get "${apt_opts[@]}" install -y \
    --no-install-recommends \
    binutils \
    g++-12 \
    gcc-12 \
    libgmp-dev \
    libntl-dev \
    ninja-build

for compiler in gcc-12 g++-12; do
    version=$("${compiler}" -dumpfullversion -dumpversion)
    if [[ "${version}" != 12 && "${version}" != 12.* ]]; then
        echo "Unexpected ${compiler} version: ${version}" >&2
        exit 1
    fi
done
