pkgname=catproccpuinfogrepmhz
pkgver=0.1.0
pkgrel=1
pkgdesc="A tool for monitoring CPU frequencies and power usage."
arch=('x86_64')
license=('GPL')
url="https://github.com/ivfiev/catproccpuinfogrepmhz"
depends=()
makedepends=('gcc' 'make')
source=("catproccpuinfogrepmhz.c" "Makefile")
md5sums=('68d7ec2df41f00fcbaae9cfb962cf34a'
         '906c69d6641f6ccd3ea65f74c0a549e8')

build() {
		make
}

package() {
    install -Dm755 "$srcdir/catproccpuinfogrepmhz" "$pkgdir/usr/bin/catproccpuinfogrepmhz"
}