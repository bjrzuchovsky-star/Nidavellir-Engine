#pragma once

// Source engine formats from the command line (docs/model-import.md):
//   ts_admin --import-gma <file.gma|.bin> --claim <dir> [--dest <rel>]
//            [--only models,materials,sound,other] [--list]
//   ts_admin --ack-rights <claim> [<asset rel>] [--revoke]
namespace ts {
namespace tombstone {
int run_import_gma_cli(int argc, char** argv);
int run_ack_rights_cli(int argc, char** argv);
}  // namespace tombstone
}  // namespace ts
