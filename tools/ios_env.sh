# Sourced from the repository root by tools/build_ios.sh and tools/wonder_push_ios.sh: the signing team, bundle id and
# device. Each comes from the environment, else from a NAME=value line in private/ios.env (kept out of git):
#   NX_IOS_TEAM=<Apple development team id>     (Xcode > Settings > Accounts, or the OU of your Apple Development cert)
#   NX_IOS_DEVICE=<the phone's UDID>            (xcrun devicectl list devices)
#   NX_IOS_BUNDLE_ID=<bundle id>                (optional, default com.wonderrecomp.<team>)
#   NX_IOS_JOBS=<parallel compiles>             (optional)

if [ -f private/ios.env ]; then
    while IFS='=' read -r key value; do
        case "$key" in
            NX_IOS_TEAM | NX_IOS_DEVICE | NX_IOS_BUNDLE_ID | NX_IOS_JOBS)
                [ -n "${!key:-}" ] || printf -v "$key" '%s' "$value" ;;
        esac
    done < private/ios.env
fi

# A bundle id must be unique among all Apple developer accounts, so the default carries the team.
if [ -z "${NX_IOS_BUNDLE_ID:-}" ] && [ -n "${NX_IOS_TEAM:-}" ]; then
    NX_IOS_BUNDLE_ID=com.wonderrecomp.$(printf '%s' "$NX_IOS_TEAM" | tr 'A-Z' 'a-z')
fi

# ios_need <variable> <what it is>: stop with a hint when a setting is missing.
ios_need() {
    if [ -z "${!1:-}" ]; then
        echo "$1 is not set: $2. Set it in the environment or as a $1=... line in private/ios.env." >&2
        exit 2
    fi
}
