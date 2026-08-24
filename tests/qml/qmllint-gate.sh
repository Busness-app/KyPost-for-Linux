#!/usr/bin/env bash
#
# Fails when a .qml file assigns a property the target type does not have.
#
# WHY THIS EXISTS
#
# A security pass added `textFormat: Text.PlainText` to a PillTab and to a
# QQC2 CheckBox. Neither type has that property, so both files failed to load
# -- and each one took its whole root down with it. The app did not start at
# all, and nothing caught it: the C++ tests never load QML, and the QML tests
# only instantiate a handful of leaf components, never DesktopRoot or
# MobileRoot. It was found by running the binary by hand.
#
# WHY IT IS NARROW
#
# qmllint emits ~900 warnings over this tree, almost all `unqualified` (the
# house style reaches singletons and ids directly) plus every member access on
# the C++-registered singletons. Those singletons come from
# qmlRegisterSingletonInstance against a plain qrc, so `com.kysecurity.mail` ships
# no qmltypes and qmllint cannot resolve any of them. Gating on the full
# output would be gating on noise.
#
# So this gates on ONE category, missing-property, minus the types qmllint
# provably cannot see. Everything left is a type it CAN resolve -- Qt's own,
# and this repo's own .qml components -- which is exactly where the bug was.
#
# To make this unnecessary, port app/qml to qt_add_qml_module so the
# singletons carry type information; then the exclusion list below can go and
# the other categories become worth turning on.

set -uo pipefail

QMLLINT="${1:?usage: qmllint-gate.sh <qmllint> <qml-dir>}"
QML_DIR="${2:?usage: qmllint-gate.sh <qmllint> <qml-dir>}"

# Types with no qmltypes, so "member not found" on them says nothing. These
# are the C++ singletons registered in main.cpp, the QObject/QQmlApplication
# they are seen as, and the interceptor registered from C++.
#
# Adding a C++-registered type means adding it here. That is deliberate
# friction: it is a prompt to give the type real qmltypes instead.
UNRESOLVABLE='"(Pairing|MailApp|Theme|General|AppLock|PgpQr|Contacts|QObject|QQmlApplication|DesktopRoot|MobileRoot|RemoteContentInterceptor)"'

mapfile -t files < <(find "$QML_DIR" -name '*.qml' | sort)

findings=$("$QMLLINT" -I "$QML_DIR" "${files[@]}" 2>&1 \
    | grep -E '\[missing-property\]' \
    | grep -Ev "not found on type $UNRESOLVABLE" || true)

if [[ -n "$findings" ]]; then
    echo "qmllint found assignments to properties that do not exist:"
    echo "$findings"
    exit 1
fi

echo "qmllint: no missing-property findings on resolvable types"
