#!/usr/bin/env bash
set -euo pipefail

printf '%-24s %8s %8s %8s\n' "APP" "RSS MiB" "PSS MiB" "CPU %"

report_process() {
    local label=$1
    local pattern=$2
    local pid
    pid=$(pgrep -f -o -- "${pattern}" || true)
    if [[ -z "${pid}" ]]; then
        printf '%-24s %8s %8s %8s\n' "${label}" "—" "—" "stopped"
        return
    fi
    local rss cpu pss
    read -r rss cpu < <(ps -p "${pid}" -o rss=,%cpu=)
    pss=$(awk '/^Pss:/{print $2}' "/proc/${pid}/smaps_rollup")
    awk -v label="${label}" -v rss="${rss}" -v pss="${pss}" -v cpu="${cpu}" \
        'BEGIN { printf "%-24s %8.1f %8.1f %8s\n", label, rss/1024, pss/1024, cpu }'
}

report_process "PasteShelf (Python/QML)" '/usr/bin/python3 -m pasteshelf'
report_process "PasteShelf Native" '/pasteshelf-native($| )'
