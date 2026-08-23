#pragma once
// Entry points for the three-layer benchmarks (design 02 §2.8), hosted by lightnfs-ctl
// as `lightnfs-ctl bench <echo|nullrpc|fullpath> [args…]`. Each parses argv like a
// standalone tool (argv[0] = the subcommand name) and terminates the process via
// _exit() when the run completes — reactors may still be parked in accept, and
// bench_nullrpc carries the phase-0 exit-gate code (2 on a sub-100k single-reactor
// run) out through that exit status.
namespace lnfs::bench {

int echo_main(int argc, char** argv);
int nullrpc_main(int argc, char** argv);
int fullpath_main(int argc, char** argv);

}  // namespace lnfs::bench
