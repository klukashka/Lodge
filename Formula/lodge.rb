class Lodge < Formula
  desc "A place for wanderers to rest"
  homepage "https://github.com/klukashka/Lodge"
  # For publishing in a tap, set this to a real GitHub release tarball URL + sha256.
  # For local testing without GitHub, set `url` to a file:// tarball you create from your working tree.
  url "https://github.com/klukashka/Lodge/archive/refs/tags/v0.2.0.tar.gz"
  sha256 "REPLACE_WITH_TARBALL_SHA256"
  license "MIT"
  head "https://github.com/klukashka/Lodge.git", branch: "main"

  def install
    system "make", "VERSION=#{version}"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    assert_predicate bin/"lodge", :exist?
    assert_predicate bin/"lodge-bin", :exist?
    assert_match version.to_s, shell_output("#{bin}/lodge-bin --version").strip
  end
end

