[CmdletBinding()]
param(
    [switch]$Check,
    [string]$ClangFormat
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project_root = Split-Path -Parent $PSScriptRoot

# clang-format 출력은 UTF-8이다. 콘솔 코드 페이지(CP949 등)로 읽으면 한글이 깨진 채 다시 쓰이므로 고정한다.
$previous_output_encoding = [Console]::OutputEncoding
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

# clang-format는 중첩 초기화 목록의 마지막 값과 닫는 중괄호를 같은 줄에 놓기도 한다.
# 앱 스타일 검사와 맞추기 위해 여러 줄 중괄호의 끝을 독립된 줄로 정규화한다.
function Convert-MultilineBraceClosures {
    param([string]$Text)

    $tokens = [regex]::Matches(
        $Text,
        '//[^\r\n]*|/\*[\s\S]*?\*/|R"(?<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)\k<delimiter>"|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*''|[{}]')
    $openings = [Collections.Generic.Stack[int]]::new()
    $insertions = [Collections.Generic.List[object]]::new()

    foreach ($token in $tokens) {
        if ($token.Value -eq '{') {
            $openings.Push($token.Index)
        }
        elseif ($token.Value -eq '}' -and $openings.Count -gt 0) {
            $opening = $openings.Pop()
            $line_start = $Text.LastIndexOf("`n", $token.Index) + 1

            if ($opening -ge $line_start) {
                continue
            }

            $prefix = $Text.Substring($line_start, $token.Index - $line_start)
            if (-not [string]::IsNullOrWhiteSpace($prefix)) {
                $opening_start = $Text.LastIndexOf("`n", $opening) + 1
                $opening_prefix = $Text.Substring($opening_start, $opening - $opening_start)
                $indent = [regex]::Match($opening_prefix, '^ *').Value
                $padding = [regex]::Match($prefix, '[ \t]*$').Length
                $insertions.Add(@{ Index = $token.Index - $padding; Remove = $padding; Text = "`n$indent" })
            }
        }
    }

    # 뒤에서 삽입하면 앞쪽 토큰의 문자 위치를 다시 계산할 필요가 없다.
    for ($index = $insertions.Count - 1; $index -ge 0; $index--) {
        $insertion = $insertions[$index]
        $Text = $Text.Remove($insertion.Index, $insertion.Remove).Insert($insertion.Index, $insertion.Text)
    }

    return $Text.Replace("`r`n", "`n").Replace("`n", "`r`n")
}

if (-not $ClangFormat) {
    $format_command = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($format_command) {
        $ClangFormat = $format_command.Source
    }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $ClangFormat = & $vswhere -latest -products '*' -find 'VC/Tools/Llvm/x64/bin/clang-format.exe' | Select-Object -First 1
        }
    }
}
if (-not $ClangFormat -or -not (Test-Path -LiteralPath $ClangFormat)) {
    throw 'Install clang-format 22, or pass -ClangFormat with its executable path.'
}
$format_version = & $ClangFormat --version
if ($LASTEXITCODE -ne 0 -or $format_version -notmatch 'clang-format version 22\.') {
    throw "clang-format 22 is required: $format_version"
}

$source_directories = @('src', 'tests') | ForEach-Object { Join-Path $project_root $_ } | Where-Object { Test-Path -LiteralPath $_ }
$source_files = Get-ChildItem -LiteralPath $source_directories -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.h') }
foreach ($source_file in $source_files) {
    $formatted_lines = & $ClangFormat $source_file.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "Formatting failed: $($source_file.FullName)"
    }

    $formatted = Convert-MultilineBraceClosures -Text (($formatted_lines -join "`n") + "`n")
    $original = [IO.File]::ReadAllText($source_file.FullName)
    if ($Check -and $original -cne $formatted) {
        throw "Formatting differs: $($source_file.FullName). Run scripts/format.ps1."
    }
    if (-not $Check -and $original -cne $formatted) {
        [IO.File]::WriteAllText($source_file.FullName, $formatted, [Text.UTF8Encoding]::new($false))
    }
}
[Console]::OutputEncoding = $previous_output_encoding
Write-Output "Formatting passed for $($source_files.Count) source files."
