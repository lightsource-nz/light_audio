# Mutation check for light_audio's PCM -> duty conversion.
#
# WHY THIS EXISTS: every way of getting this conversion wrong is audible but none are visible
# in the code, and testing it on hardware means listening to a piezo and guessing. The first
# version of test_audio_convert.c passed against four broken implementations. Three of those
# were genuine redundancy between the clamps; the fourth -- dividing where the code shifts --
# was a real blind spot, and closing it took a case that counts how many input codes map to
# each output value.
#
# Each mutant should make ctest go red. One SURVIVES for a good reason; see the note at the
# end before assuming the suite is weak.
#
# HOW IT WORKS: patches src/audio.c in place, rebuilds, runs ctest, restores. The source is
# always restored, including on Ctrl-C, but it IS edited in your working tree while this runs.
#
# USAGE:  pwsh test/mutants.ps1 [-BuildDir <path>]
param(
        [string]$BuildDir = (Join-Path $PSScriptRoot '..\..\..\build-host')
)

$ErrorActionPreference = 'Stop'
$src = (Resolve-Path (Join-Path $PSScriptRoot '..\src\audio.c')).Path

if (-not (Test-Path $BuildDir)) {
        Write-Error "no build directory at $BuildDir -- configure a HOST build first, or pass -BuildDir"
}
$BuildDir = (Resolve-Path $BuildDir).Path

# single-line search strings only -- see the note in rend's copy of this script
$mutants = @(
 @('attenuate towards zero',   'int32_t duty = centred + LIGHT_AUDIO_DUTY_SILENCE;', 'int32_t duty = (centred + LIGHT_AUDIO_DUTY_SILENCE) * (int32_t)volume / LIGHT_AUDIO_VOLUME_MAX;'),
 @('no re-centring',           'int32_t duty = centred + LIGHT_AUDIO_DUTY_SILENCE;', 'int32_t duty = centred;'),
 @('divide instead of shift',  'centred = sample >> 8;', 'centred = sample / 256;'),
 @('wrong U8 bias',            'centred = (sample & 0xFF) - 128;', 'centred = (sample & 0xFF) - 127;'),
 @('no input clamp',           'if(sample < -32768) sample = -32768;', 'if(0) sample = -32768;'),
 @('no output clamp',          'if(duty > LIGHT_AUDIO_DUTY_MAX) duty = LIGHT_AUDIO_DUTY_MAX;', 'if(0) duty = LIGHT_AUDIO_DUTY_MAX;'),
 @('volume not clamped',       'if(volume > LIGHT_AUDIO_VOLUME_MAX)', 'if(0)'),
 @('shift by 7 (double gain)', 'centred = sample >> 8;', 'centred = sample >> 7;')
)

$original = Get-Content $src -Raw
$restored = $false
function Restore-Source {
        if (-not $script:restored) {
                Set-Content $script:src -Value $script:original -NoNewline
                $script:restored = $true
                Write-Host "source restored"
        }
}
trap { Restore-Source; break }

try {
        Write-Host "=== baseline (unmutated) ==="
        & cmake --build $BuildDir --target test_audio_convert 2>&1 | Out-Null
        & ctest --test-dir $BuildDir -R '^light_audio\.' 2>&1 | Select-Object -Last 1

        Write-Host "`n=== mutants (each SHOULD fail) ==="
        foreach ($m in $mutants) {
                $name, $find, $replace = $m
                $patched = $original.Replace($find, $replace)
                if ($patched -eq $original) {
                        "{0,-28} !! search string not found -- has audio.c moved on?" -f $name
                        continue
                }
                Set-Content $src -Value $patched -NoNewline
                $build = & cmake --build $BuildDir --target test_audio_convert 2>&1
                if ($LASTEXITCODE -ne 0) {
                        "{0,-28} -- did not compile (not a useful mutant)" -f $name
                } else {
                        $out = & ctest --test-dir $BuildDir -R '^light_audio\.' 2>&1
                        $line = ($out | Select-String 'tests passed' | Select-Object -First 1)
                        $verdict = if ($LASTEXITCODE -ne 0) { 'caught' } else { '*** SURVIVED ***' }
                        "{0,-28} -> {1}  ({2})" -f $name, $verdict, ($line -replace '\s+', ' ').Trim()
                }
                Set-Content $src -Value $original -NoNewline
        }
} finally {
        Restore-Source
        & cmake --build $BuildDir --target test_audio_convert 2>&1 | Out-Null
}

# KNOWN SURVIVOR: 'no output clamp'. The input clamp already bounds `centred` to -128..127, so
# adding LIGHT_AUDIO_DUTY_SILENCE cannot leave 0..255 and the output clamp is unreachable. It
# is kept as defence for a future encoding that widens the intermediate range, and audio.c
# says so at the site. A surviving mutant on genuinely unreachable code is the correct result,
# not a gap in the suite.
