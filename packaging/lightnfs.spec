# RPM spec (plan doc 10 §4.5), driven by packaging/make_rpm.sh which stages the
# build tree itself — the spec only packages the staged files, so no network or
# source tarball is needed at rpmbuild time.
Name:           lightnfs
Version:        %{lnfs_version}
Release:        1%{?dist}
Summary:        Userspace NFS gateway (NFSv3 + NFSv4.1/4.2)
License:        MIT
BuildArch:      %{_arch}

%description
io_uring-based userspace NFS server with a local filesystem backend,
Prometheus metrics and a unix-socket admin interface (lightnfs-ctl).

%install
cp -a %{lnfs_stage}/. %{buildroot}/

%files
%{_bindir}/lightnfsd
%{_bindir}/lightnfs-ctl
%{_bindir}/lightnfs-fh
%config(noreplace) %{_sysconfdir}/lightnfs/lightnfs.toml.example
%{_libdir}/systemd/system/lightnfs.service
%doc %{_docdir}/lightnfs/README.md
%doc %{_docdir}/lightnfs/deployment.md

%changelog
* Thu Aug 28 2026 lightnfs maintainers <lightnfs@example.invalid> - 1.3.0-1
- Initial packaged release: observability + ops milestone (plan doc 10 §3/§4).
