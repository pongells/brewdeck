#pragma once
// The public key that release descriptions are signed with (ECDSA P-256; tools/sign-update.sh, the release workflow).
// A firmware accepts only releases signed by the private half of this key, which is kept outside the repository (a
// GitHub secret for the release workflow). A fork that publishes its own releases puts its own public key here.
static const char UPDATE_PUBKEY_PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEl6N5GdO11CLf+SS/ony4NdBQvYDv\n"
    "3IibSKIOdDyJETGpd+qwvN8UYej39cTqreeQyYFCtYNe5MR7I3j5YUBJqg==\n"
    "-----END PUBLIC KEY-----\n";
// where releases are published: https://github.com/<UPDATE_REPO>/releases
#ifndef UPDATE_REPO
#define UPDATE_REPO "pongells/brewdeck"
#endif
