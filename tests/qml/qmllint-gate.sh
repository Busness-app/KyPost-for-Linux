#!/usr/bin/env bash
#
# Two static gates over app/qml, neither of which needs the app to run:
#
#   1. a .qml file assigns a property the target type does not have
#   2. a Text or Label renders something other than a literal without saying
#      what text format it is
#
# WHY THE FIRST ONE EXISTS
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

status=0

if [[ -n "$findings" ]]; then
    echo "qmllint found assignments to properties that do not exist:"
    echo "$findings"
    status=1
else
    echo "qmllint: no missing-property findings on resolvable types"
fi

# ---- gate 2: a Text that is not a literal declares its format ------------
#
# Text.textFormat defaults to Text.AutoText, which runs Qt::mightBeRichText()
# and hands anything tag-shaped to the rich-text parser. That parser fetches
# <img src> over the QML engine's own QNetworkAccessManager -- a different
# stack from the mail viewer's WebEngineView, so neither settings.
# autoLoadImages nor RemoteContentInterceptor can see it. A subject, a folder
# name or a timestamp straight off the relay beacons the moment it lays out.
#
# This gates the class rather than the instance because the instance was
# invisible: the last one found (F-01-8) was the timestamp in MobileRoot's
# inbox delegate, the only one of five Texts there without textFormat,
# rendering the raw wire atUtc string. No test that runs the app could catch
# it -- QmlTests cannot load MobileRoot -- so this reads the source instead.
#
# The rule: a Text or Label with no explicit textFormat may bind only a string
# literal, or an i18n()/qsTr() of literals. A property, a call, or a value
# interpolated into a translated string all have to say what format they are.
# Blunt on purpose -- it does not trace where a value came from, so it costs a
# line on a little chrome and in exchange needs no allowlist to stay quiet.
#
# Not a QML parser: it reads indented blocks and strips literals and comments
# before counting braces. Anything it cannot read that way -- a Text declared
# on one line, a text: at an unexpected indent -- it reports rather than
# skips, so the way past it is to write the file in the house style.
offenders=$(QML_DIR="$QML_DIR" python3 - <<'PY'
import os, pathlib, re

OPENS = re.compile(r'(^|[\s:])(Text|Label)\s*\{')
CLOSES_LINE = re.compile(r'(^|[\s:])(Text|Label)\s*\{\s*$')
LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')
TRANSLATION = re.compile(r'\b(i18ncp|i18nc|i18np|i18n|qsTrId|qsTr)\b')

def bare(line):
    # Literals first: stripping comments first would cut a line at the "//"
    # inside a URL string.
    return re.sub(r'//.*$', '', LITERAL.sub('', line))

for path in sorted(pathlib.Path(os.environ["QML_DIR"]).rglob("*.qml")):
    lines = path.read_text().split("\n")
    for i, line in enumerate(lines):
        if not OPENS.search(bare(line)):
            continue
        if not CLOSES_LINE.search(bare(line)):
            print(f"{path}:{i + 1}: declared on one line, which this check cannot read")
            continue
        indent = len(line) - len(line.lstrip())
        depth, end = 0, i
        for end in range(i, len(lines)):
            depth += bare(lines[end]).count("{") - bare(lines[end]).count("}")
            if end > i and depth <= 0:
                break
        block = lines[i:end + 1]
        # At this block's own indent, not a nested child's -- a Text whose
        # child pins textFormat has still said nothing about itself.
        if any(re.match(r' {%d}textFormat\s*:' % (indent + 4), l) for l in block):
            continue
        at = next((n for n, l in enumerate(block)
                   if re.match(r' {%d}text\s*:' % (indent + 4), l)), None)
        if at is None:
            print(f"{path}:{i + 1}: no text: of its own, so a caller supplies it")
            continue
        expr = bare(block[at]).split(":", 1)[1]
        n = at
        while expr.count("(") > expr.count(")") and n + 1 < len(block):
            n += 1
            expr += " " + bare(block[n])
        if re.search(r'[A-Za-z_$]', TRANSLATION.sub("", expr)):
            print(f"{path}:{i + 1}: {block[at].strip()}")
PY
)

if [[ -n "$offenders" ]]; then
    echo "Text/Label rendering something a literal cannot account for, with no textFormat:"
    echo "$offenders"
    status=1
else
    echo "textFormat: every data-bound Text and Label declares its format"
fi

exit $status
