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
#   two sources now: the core (audio.c) and the PWM driver (audio_pwm.c). Each mutant names the
# one it patches, and every file touched is backed up and restored, so a run that dies partway
# cannot leave either of them mutated
$sources = @{
        'audio'     = (Resolve-Path (Join-Path $PSScriptRoot '..\src\audio.c')).Path
        'audio_pwm' = (Resolve-Path (Join-Path $PSScriptRoot '..\src\audio_pwm.c')).Path
}

if (-not (Test-Path $BuildDir)) {
        Write-Error "no build directory at $BuildDir -- configure a HOST build first, or pass -BuildDir"
}
$BuildDir = (Resolve-Path $BuildDir).Path

# single-line search strings only -- see the note in rend's copy of this script
$mutants = @(
 @('attenuate towards zero','audio',   'int32_t duty = centred + LIGHT_AUDIO_DUTY_SILENCE;', 'int32_t duty = (centred + LIGHT_AUDIO_DUTY_SILENCE) * (int32_t)volume / LIGHT_AUDIO_VOLUME_MAX;'),
 @('no re-centring','audio',           'int32_t duty = centred + LIGHT_AUDIO_DUTY_SILENCE;', 'int32_t duty = centred;'),
 @('divide instead of shift','audio',  'centred = sample >> 8;', 'centred = sample / 256;'),
 @('wrong U8 bias','audio',            'centred = (sample & 0xFF) - 128;', 'centred = (sample & 0xFF) - 127;'),
 @('no input clamp','audio',           'if(sample < -32768) sample = -32768;', 'if(0) sample = -32768;'),
 @('no output clamp','audio',          'if(duty > LIGHT_AUDIO_DUTY_MAX) duty = LIGHT_AUDIO_DUTY_MAX;', 'if(0) duty = LIGHT_AUDIO_DUTY_MAX;'),
 @('volume not clamped','audio',       'if(volume > LIGHT_AUDIO_VOLUME_MAX)', 'if(0)'),
 @('shift by 7 (double gain)','audio', 'centred = sample >> 8;', 'centred = sample >> 7;'),
 #   the playback lifecycle, covered by test_audio_playback.c. These break sequencing rather
 # than arithmetic, so the symptom is not a wrong sound but a use-after-free, a leak, or a
 # transducer left sounding -- none of which a listening test would reliably catch, and the
 # last of which was a real bug this suite found
 #   note the indentation carried in these anchors: .Replace() is global, so a 16-space
 # `_release_buffer(dev);` picks out the one on the refused-submit path while leaving the
 # three at other depths alone. Where an anchor genuinely appears twice at the same depth the
 # mutant is named for what patching BOTH does
 @('buffer freed while DMA reads it','audio', '                if(dev->playing && !dev->driver_ctx->driver->busy(dev)) {', '                if(dev->playing) {'),
 @('conversion buffer never freed','audio',  '        if(!dev->owned_buffer)', '        if(1)'),
 @('refused submit leaks its buffer','audio', '                _release_buffer(dev);', '                ;'),
 @('zero-copy taken at any volume','audio',  '        return format->encoding == LIGHT_AUDIO_PCM_U8 && volume == LIGHT_AUDIO_VOLUME_MAX;', '        return format->encoding == LIGHT_AUDIO_PCM_U8;'),
 @('busy device is interrupted','audio',     '        if(dev->driver_ctx->driver->busy(dev))', '        if(0)'),
 @('stereo accepted','audio',                '        if(format.channels > 1) {', '        if(0) {'),
 @('unknown encoding accepted','audio',      '        if(format.encoding != LIGHT_AUDIO_PCM_U8 && format.encoding != LIGHT_AUDIO_PCM_S16) {', '        if(0) {'),
 @('the driver is never stopped','audio',    '        dev->driver_ctx->driver->stop(dev);', '        ;'),
 @('stop does not silence a tone','audio',   '                dev->driver_ctx->driver->tone(dev, 0);' + "`r`n" + '        _release_buffer(dev);', '        _release_buffer(dev);'),
 @('indefinite tone expires immediately','audio', '                if(dev->toning && dev->tone_end_ms', '                if(dev->toning && (1 || dev->tone_end_ms)'),
 @('tone duration never expires','audio',    '                                && (int32_t)(now - dev->tone_end_ms) >= 0) {', '                                && 0) {'),
 #   the PWM driver, covered by test_audio_pwm.c against a --wrap'd fake peripheral. Every one of
 # these compiles cleanly, passes review, and is AUDIBLE: a click on the first sample, a hiss
 # from a floating pin, or a piezo left holding whatever level it stopped on
 @('output not parked silent on open','audio_pwm',
   '                light_platform_pwm_set_duty(state->pwm, LIGHT_AUDIO_DUTY_SILENCE);', '                ;'),
 @('open configures the wrong wrap','audio_pwm',
   '                light_platform_pwm_configure(state->pwm, LIGHT_AUDIO_DUTY_MAX, 1);',
   '                light_platform_pwm_configure(state->pwm, AUDIO_PWM_TONE_WRAP, 1);'),
 @('tone at full duty instead of half','audio_pwm',
   '        light_platform_pwm_set_duty(state->pwm, AUDIO_PWM_TONE_WRAP / 2);',
   '        light_platform_pwm_set_duty(state->pwm, AUDIO_PWM_TONE_WRAP);'),
 @('0 Hz leaves the pin floating','audio_pwm',
   '                light_platform_pwm_release_pin(state->pwm, false);' + "`r`n" + '                return;',
   '                return;'),
 @('0 Hz releases the pin HIGH','audio_pwm',
   '                light_platform_pwm_release_pin(state->pwm, false);' + "`r`n" + '                return;',
   '                light_platform_pwm_release_pin(state->pwm, true);' + "`r`n" + '                return;'),
 @('teardown closes without releasing','audio_pwm',
   '                light_platform_pwm_release_pin(state->pwm, false);' + "`r`n" + '                light_platform_pwm_close(state->pwm);',
   '                light_platform_pwm_close(state->pwm);'),
 @('teardown leaks the PWM block','audio_pwm',
   '                light_platform_pwm_close(state->pwm);', '                ;'),
 @('missing PWM is not tolerated','audio_pwm', '        if(!state->pwm)', '        if(0)')
)

$targets = @('test_audio_convert', 'test_audio_playback', 'test_audio_pwm')

# every source is captured up front, so a restore never depends on which one a run died on
$originals = @{}
foreach ($k in $sources.Keys) { $originals[$k] = Get-Content $sources[$k] -Raw }
$restored = $false
function Restore-Sources {
        if (-not $script:restored) {
                foreach ($k in $script:sources.Keys) {
                        Set-Content $script:sources[$k] -Value $script:originals[$k] -NoNewline
                }
                $script:restored = $true
                Write-Host "sources restored"
        }
}
trap { Restore-Sources; break }

try {
        Write-Host "=== baseline (unmutated) ==="
        & cmake --build $BuildDir --target @targets 2>&1 | Out-Null
        & ctest --test-dir $BuildDir -R '^light_audio\.' 2>&1 | Select-Object -Last 1

        Write-Host "`n=== mutants (each SHOULD fail) ==="
        foreach ($m in $mutants) {
                $name, $which, $find, $replace = $m
                $src = $sources[$which]
                $original = $originals[$which]
                $patched = $original.Replace($find, $replace)
                if ($patched -eq $original) {
                        "{0,-40} !! anchor not found -- has {1}.c moved on?" -f $name, $which
                        continue
                }
                Set-Content $src -Value $patched -NoNewline
                & cmake --build $BuildDir --target @targets 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) {
                        "{0,-40} -> killed (build)" -f $name
                } else {
                        $out = & ctest --test-dir $BuildDir -R '^light_audio\.' 2>&1
                        $line = ($out | Select-String 'tests passed' | Select-Object -First 1)
                        $verdict = if ($LASTEXITCODE -ne 0) { 'caught' } else { '*** SURVIVED ***' }
                        "{0,-40} -> {1}  ({2})" -f $name, $verdict, ($line -replace '\s+', ' ').Trim()
                }
                Set-Content $src -Value $original -NoNewline
        }
} finally {
        Restore-Sources
        & cmake --build $BuildDir --target @targets 2>&1 | Out-Null
}

# KNOWN SURVIVOR: 'no output clamp'. The input clamp already bounds `centred` to -128..127, so
# adding LIGHT_AUDIO_DUTY_SILENCE cannot leave 0..255 and the output clamp is unreachable. It
# is kept as defence for a future encoding that widens the intermediate range, and audio.c
# says so at the site. A surviving mutant on genuinely unreachable code is the correct result,
# not a gap in the suite.
