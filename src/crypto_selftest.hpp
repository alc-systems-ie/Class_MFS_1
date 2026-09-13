#pragma once

namespace alc::crypto
{

  /**
   * @brief Prove the on-target crypto backend against the generated vectors.
   *
   * Runs every command vector and the time-sync vector from
   * tools/gen_access_vectors.py through access_keys on PSA. The host tests prove
   * the same derivations on OpenSSL; only this proves CRACEN.
   *
   * @return 0 if every vector matches; -EBADMSG on the first mismatch. The caller
   *         must refuse to process commands on failure - a backend that disagrees
   *         with the app would reject every genuine command, or worse.
   */
  int RunSelfTest();

}
