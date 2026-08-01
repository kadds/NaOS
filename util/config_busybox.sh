#!/bin/sh
set -eu

source_dir=$1
build_dir=$2
config_fragment=$3
make_program=${4:-make}
host_cc=${5:-cc}

"${make_program}" -C "${source_dir}" O="${build_dir}" HOSTCC="${host_cc}" allnoconfig

while IFS= read -r entry || [ -n "${entry}" ]; do
    case "${entry}" in
        ""|\#*)
            continue
            ;;
        CONFIG_*=*)
            key=${entry%%=*}
            sed -i \
                -e "s|^# ${key} is not set$|${entry}|" \
                -e "s|^${key}=.*$|${entry}|" \
                "${build_dir}/.config"
            ;;
    esac
done < "${config_fragment}"

yes '' | "${make_program}" -C "${source_dir}" O="${build_dir}" HOSTCC="${host_cc}" oldconfig
