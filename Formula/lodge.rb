class Lodge < Formula
  desc "A place for wanderers to rest"
  homepage "https://github.com/klukashka/Lodge"
  url "https://github.com/klukashka/Lodge/archive/refs/tags/v0.2.0.tar.gz"
  sha256 "REPLACE_WITH_TARBALL_SHA256"
  license "MIT"
  head "https://github.com/klukashka/Lodge.git", branch: "main"

  def install
    system "make"
    system "make", "install", "PREFIX=#{prefix}"
  end

  test do
    assert_predicate bin/"lodge", :exist?
    assert_predicate bin/"lodge-bin", :exist?
  end
end

