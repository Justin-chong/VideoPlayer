$cmakePaths = @(
    "C:\Qt\Tools\CMake_64\bin\cmake.exe",
    "C:\Qt\Tools\CMake_64\bin",
    "C:\Program Files\CMake\bin\cmake.exe"
)
foreach ($p in $cmakePaths) {
    if (Test-Path $p) {
        Write-Host "FOUND cmake: $p"
    }
}

# Also search for cmake.exe in common Qt install locations
Get-ChildItem "C:\Qt" -Recurse -Filter "cmake.exe" -ErrorAction SilentlyContinue | Select-Object -First 5 -ExpandProperty FullName

# Check QTDIR env var
Write-Host "QTDIR env: $env:QTDIR"

# Check if cmake is in PATH
where.exe cmake 2>$null
