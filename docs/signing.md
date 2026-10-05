# Code signing (SignPath Foundation) and releases

Windows Defender flags the unsigned sopt-opbench.exe (OpBench.exe since 0.3.0) as Trojan:Win32/Sabsik.FL.A!ml (a machine
learning false positive on unsigned programs). The repository is public and GPL-3.0, so SignPath
Foundation signs it for free. Their review is done by people and can take days.

## Already in the repository
- `.github/workflows/release.yml`: a tag `v*` (or a manual run, which makes a draft release)
  builds sopt / sopt-fx (Windows and Linux) and the Windows tools, signs the Windows files when
  the SignPath secret exists, and attaches `SweetOpt-<v>-windows-x64.zip`, `SweetOpt-<v>-linux-x64.tar.gz`,
  `GPU-Blueprint-<v>.zip`, `sopt-windows-tools-<v>.zip` and `SHA256SUMS.txt` to a GitHub Release.
- Version info (name, publisher CeeJay.dk, version) and a manifest on the executables
  (`tools/windows/version.rc.in`, `tools/windows/app.manifest`).
- README.md: the code signing policy and privacy sections SignPath asks for.

## The owner's steps
1. GitHub: enable two-factor authentication on the account (SignPath requires it).
2. Make a first (unsigned) release so there is a download page: push a tag, e.g.
   `git tag v0.1.0 && git push origin v0.1.0` (from the branch you want released; normally main
   after merging).
3. Apply at https://signpath.org/apply:
   - Repository: https://github.com/CeeJayDK/sopt
   - Homepage: https://github.com/CeeJayDK/sopt
   - Download: https://github.com/CeeJayDK/sopt/releases
   - Privacy policy: https://github.com/CeeJayDK/sopt#privacy
   - Code signing policy: https://github.com/CeeJayDK/sopt#code-signing-policy
   - Build system: GitHub Actions (`.github/workflows/release.yml`)
   - Description: "Shader superoptimizer for ReShade FX effects; the signed files are its
     command-line tools (sopt, sopt-fx) and small Windows tools (a GPU instruction benchmark and a
     benchmark harness)."
4. After approval, in GitHub (repository Settings > Secrets and variables > Actions):
   - secret `SIGNPATH_API_TOKEN` (the CI user's API token from SignPath)
   - variables `SIGNPATH_ORGANIZATION_ID`, `SIGNPATH_PROJECT_SLUG`, `SIGNPATH_POLICY_SLUG`
     (`release-signing`, or `test-signing` first)
5. In SignPath, the project's artifact configuration (the uploaded artifact is a zip that holds
   the Windows release zips):

```xml
<?xml version="1.0" encoding="utf-8"?>
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <zip-file>
    <zip-file path="SweetOpt-*-windows-x64.zip">
      <pe-file-set>
        <include path="bin/sopt.exe"/>
        <include path="bin/sopt-fx.exe"/>
        <for-each><authenticode-sign/></for-each>
      </pe-file-set>
    </zip-file>
    <zip-file path="GPU-Blueprint-*.zip">
      <pe-file-set>
        <include path="bin/OpBench.exe"/>
        <include path="bin/TexBench.exe"/>
        <include path="bin/ShaderInfo.exe"/>
        <for-each><authenticode-sign/></for-each>
      </pe-file-set>
    </zip-file>
    <zip-file path="sopt-windows-tools-*.zip">
      <pe-file-set>
        <include path="bin/sopt-host.exe"/>
        <include path="bin/sopt-fxc.exe"/>
        <include path="bin/sopt-timer.addon64"/>
        <include path="bin/sopt-timer.addon32"/>
        <for-each><authenticode-sign/></for-each>
      </pe-file-set>
    </zip-file>
  </zip-file>
</artifact-configuration>
```

6. Push the next tag: the workflow signs automatically.

## Until then
Report each new build as a false positive at https://www.microsoft.com/en-us/wdsi/filesubmission
(Software developer, the zip or exe, detection name Trojan:Win32/Sabsik.FL.A!ml). It usually clears
within days, for that exact file only.
