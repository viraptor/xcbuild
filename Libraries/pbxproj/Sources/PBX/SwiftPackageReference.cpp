/**
 Copyright (c) 2026-present, Stanisław Pitucha
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#include <pbxproj/PBX/SwiftPackageReference.h>
#include <pbxproj/Context.h>
#include <plist/String.h>
#include <plist/Keys/Unpack.h>

using pbxproj::PBX::SwiftPackageReference;
using pbxproj::Context;

SwiftPackageReference::
SwiftPackageReference() :
    Object(Isa())
{
}

bool SwiftPackageReference::
parse(Context &context, plist::Dictionary const *dict, std::unordered_set<std::string> *seen, bool check)
{
    if (!Object::parse(context, dict, seen, false)) {
        return false;
    }

    auto unpack = plist::Keys::Unpack("SwiftPackageReference", dict, seen);

    auto RP = unpack.cast <plist::String> ("relativePath");

    if (!unpack.complete(check)) {
        fprintf(stderr, "%s", unpack.errorText().c_str());
    }

    if (RP != nullptr) {
        _relativePath = RP->value();
    }

    return true;
}
