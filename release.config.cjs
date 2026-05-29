const branchName = process.env.GITHUB_REF_NAME || "master";

const branches =
  branchName === "master"
    ? ["master"]
    : [
        "master",
        {
          name: branchName,
          prerelease: "dev",
        },
      ];

module.exports = {
  branches,
  tagFormat: "v${version}",
  plugins: [
    "@semantic-release/commit-analyzer",
    "@semantic-release/release-notes-generator",
    [
      "@semantic-release/github",
      {
        assets: [
          {
            path: "out/*.bin",
            label: "Firmware binary",
          },
        ],
      },
    ],
  ],
};
