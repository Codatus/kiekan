# Copyright 2026 Carl Kliefert
# Distributed under the terms of the GNU General Public License v3

EAPI=8

inherit cmake xdg

DESCRIPTION="Fast, lightweight image viewer with editing, OCR and an MCP interface for AI assistants"
HOMEPAGE="https://github.com/Codatus/kiekan"
SRC_URI="https://github.com/Codatus/kiekan/archive/v${PV}.tar.gz -> ${P}.tar.gz"

LICENSE="GPL-3+"
SLOT="0"
KEYWORDS="~amd64"
IUSE="+exif +ocr +pdf +video"

DEPEND="
	dev-qt/qtbase:6[gui,network,opengl,widgets]
	dev-qt/qtsvg:6
	media-libs/libheif:=
	media-libs/libraw:=
	media-libs/openjpeg:2=
	app-text/djvu
	media-libs/libjpeg-turbo:=
	exif? ( media-gfx/exiv2:= )
	ocr? (
		app-text/tesseract:=
		media-libs/leptonica:=
	)
	pdf? (
		dev-qt/qtwebengine:6[pdfium]
		app-text/qpdf:=
	)
	video? ( media-video/mpv:=[libmpv] )
"
RDEPEND="${DEPEND}
	dev-qt/qtimageformats:6
"
BDEPEND="
	dev-qt/qttools:6[linguist]
	virtual/pkgconfig
"

src_configure() {
	local mycmakeargs=(
		-DKIEKAN_WITH_EXIF=$(usex exif)
		-DKIEKAN_WITH_OCR=$(usex ocr)
		-DKIEKAN_WITH_PDF=$(usex pdf)
		-DKIEKAN_WITH_QPDF=$(usex pdf)
		-DKIEKAN_WITH_VIDEO=$(usex video)
	)
	cmake_src_configure
}

src_test() {
	local -x QT_QPA_PLATFORM=offscreen
	cmake_src_test
}
