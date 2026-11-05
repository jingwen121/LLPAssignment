param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('client2', 'client3', 'client4', 'client5', 'client6')]
    [string]$Player
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$invitePath = Join-Path $projectRoot "invites\coinrush-$Player.tar.gz.gpg"
if (-not (Test-Path -LiteralPath $invitePath -PathType Leaf)) {
    throw "The encrypted invite was not created: $invitePath"
}

$packageDir = Join-Path $projectRoot 'packages'
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null
$outputZip = Join-Path $packageDir "CoinRush-$Player.zip"
$stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("CoinRush-package-" + [guid]::NewGuid().ToString('N'))
$stageProject = Join-Path $stageRoot "CoinRush-$Player"

try {
    New-Item -ItemType Directory -Path $stageProject -Force | Out-Null

    # Package only source, assets, and launch files. This whitelist deliberately
    # excludes .git history, builds, executables, and every local credential.
    $directories = @('assets', 'docs', 'include', 'protocol', 'scripts', 'src', 'tests', 'vendor')
    foreach ($name in $directories) {
        $source = Join-Path $projectRoot $name
        if (Test-Path -LiteralPath $source -PathType Container) {
            Copy-Item -LiteralPath $source -Destination $stageProject -Recurse
        }
    }

    $files = @('.gitattributes', '.gitignore', 'Makefile', 'README.md', 'client.bat', 'server.bat', 'create-invite.bat')
    foreach ($name in $files) {
        $source = Join-Path $projectRoot $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination $stageProject
        }
    }

    $stagedInviteDir = Join-Path $stageProject 'invites'
    New-Item -ItemType Directory -Path $stagedInviteDir -Force | Out-Null
    Copy-Item -LiteralPath $invitePath -Destination $stagedInviteDir

    if (Test-Path -LiteralPath (Join-Path $stageProject 'credentials-private')) {
        throw 'Refusing to package private host credentials.'
    }
    if (Test-Path -LiteralPath (Join-Path $stageProject 'credentials-invites')) {
        throw 'Refusing to package imported client credentials.'
    }

    if (Test-Path -LiteralPath $outputZip) {
        Remove-Item -LiteralPath $outputZip -Force
    }
    Compress-Archive -LiteralPath $stageProject -DestinationPath $outputZip -CompressionLevel Optimal
    Write-Output "Created $outputZip"
}
finally {
    if (Test-Path -LiteralPath $stageRoot -PathType Container) {
        $resolvedStage = (Resolve-Path -LiteralPath $stageRoot).Path
        $resolvedTemp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
        if ($resolvedStage.StartsWith($resolvedTemp, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedStage -Recurse -Force
        }
    }
}
