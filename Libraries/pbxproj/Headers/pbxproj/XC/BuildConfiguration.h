/**
 Copyright (c) 2015-present, Facebook, Inc.
 All rights reserved.

 This source code is licensed under the BSD-style license found in the
 LICENSE file in the root directory of this source tree.
 */

#ifndef __pbxproj_XC_BuildConfiguration_h
#define __pbxproj_XC_BuildConfiguration_h

#include <pbxproj/PBX/FileReference.h>
#include <pbxsetting/Level.h>

namespace pbxproj { namespace XC {

class BuildConfiguration : public PBX::Object {
public:
    typedef std::shared_ptr <BuildConfiguration> shared_ptr;
    typedef std::vector <shared_ptr> vector;

private:
    std::string                     _name;
    PBX::FileReference::shared_ptr  _baseConfigurationReference;
    pbxsetting::Level               _buildSettings;
    std::vector<std::pair<std::string, std::string>> _buildSettingsRaw;

public:
    BuildConfiguration();
    ~BuildConfiguration();

public:
    inline PBX::FileReference::shared_ptr const &baseConfigurationReference() const
    { return _baseConfigurationReference; }

public:
    inline pbxsetting::Level const &buildSettings() const
    { return _buildSettings; }

    /*
     * The build settings exactly as written in the project, in file order:
     * original key (including any `[condition]` suffix) paired with the verbatim
     * value string. Unlike buildSettings(), this does not normalize setting
     * reference syntax (${X}/$X stay as written rather than becoming $(X)), so
     * it reproduces the host's PIF output byte-for-byte.
     */
    inline std::vector<std::pair<std::string, std::string>> const &buildSettingsRaw() const
    { return _buildSettingsRaw; }

public:
    inline std::string const &name() const
    { return _name; }

protected:
    bool parse(Context &context, plist::Dictionary const *dict, std::unordered_set<std::string> *seen, bool check) override;

public:
    static inline char const *Isa()
    { return ISA::XCBuildConfiguration; }
};

} }

#endif  // !__pbxproj_XC_BuildConfiguration_h
