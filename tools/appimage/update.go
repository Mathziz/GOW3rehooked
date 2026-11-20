// SPDX-License-Identifier: GPL-2.0-or-later
// AppRun for tagged Linux AppImages. AppRun.game is the entry point from linuxdeploy.
package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"syscall"
	"time"
)

const repo = "cuesta4/GOW3rehooked"
const gameImage = "gow3.AppImage"
const debugImage = "gow3_debug.AppImage"
const cache = "_rehooked_/shader_cache/recipes-hash2-aes.bin"

var currentTag, assetName string
var tagPattern = regexp.MustCompile(`^v([0-9]+)\.([0-9]+)(?:\.([0-9]+))?(?:-(alpha|beta|rc)(?:\.([0-9]+))?)?$`)
var license = regexp.MustCompile(`^licenses/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.txt$`)
var digest = regexp.MustCompile(`^sha256:[0-9a-f]{64}$`)

func version(tag string) ([5]int, bool) {
	var v [5]int
	m := tagPattern.FindStringSubmatch(tag)
	if m == nil {
		return v, false
	}
	for _, i := range []int{0, 1, 2, 4} {
		if m[i+1] != "" {
			n, err := strconv.Atoi(m[i+1])
			if err != nil {
				return v, false
			}
			v[i] = n
		}
	}
	v[3] = map[string]int{"alpha": 0, "beta": 1, "rc": 2, "": 3}[m[4]]
	return v, true
}

func newer(tag string) bool {
	next, ok := version(tag)
	current, known := version(currentTag)
	if ok && known {
		for i := range next {
			if next[i] != current[i] {
				return next[i] > current[i]
			}
		}
	}
	return false
}

func fetch(url string, limit int64, timeout time.Duration) ([]byte, error) {
	client := http.Client{Timeout: timeout, CheckRedirect: func(req *http.Request, via []*http.Request) error {
		if req.URL.Scheme != "https" || len(via) >= 10 {
			return errors.New("invalid release redirect")
		}
		return nil
	}}
	resp, err := client.Get(url)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, errors.New(resp.Status)
	}
	if resp.ContentLength > limit {
		return nil, errors.New("release response too large")
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if int64(len(data)) > limit {
		return nil, errors.New("release response too large")
	}
	return data, err
}

type asset struct {
	Name, Digest string
	Size         int64
}
type release struct {
	Tag    string `json:"tag_name"`
	Assets []asset
}

func writeFile(path string, src io.Reader, mode os.FileMode) error {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, mode)
	if err != nil {
		return err
	}
	_, err = io.Copy(f, src)
	return errors.Join(err, f.Close())
}

// Hard links also preserve the inode of a mounted AppImage. Portable filesystems can use a copy.
func backup(path, dest string, mode os.FileMode) error {
	if os.Link(path, dest) == nil {
		return nil
	}
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	return writeFile(dest, f, mode.Perm())
}

type replacement struct{ source, target, previous string }

func install(data []byte, image string, restart func() error) (err error) {
	archive, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return err
	}
	dir := filepath.Dir(image)
	stage, err := os.MkdirTemp(dir, ".gow3-update-")
	if err != nil {
		return err
	}
	var files []replacement
	committed := 0
	defer func() {
		if err != nil {
			var rollback error
			for i := committed - 1; i >= 0; i-- {
				f := files[i]
				if f.previous != "" {
					rollback = errors.Join(rollback, os.Rename(f.previous, f.target))
				} else {
					rollback = errors.Join(rollback, os.Remove(f.target))
				}
			}
			if rollback != nil {
				err = fmt.Errorf("%w; restoring files: %v (backups in %s)", err, rollback, stage)
				return
			}
		}
		os.RemoveAll(stage)
	}()
	found := make(map[string]bool)
	var total uint64
	for i, entry := range archive.File {
		if entry.FileInfo().IsDir() {
			continue
		}
		name := entry.Name
		program := name == gameImage || name == debugImage
		if !entry.Mode().IsRegular() || (!program && name != cache && !license.MatchString(name)) || found[name] || entry.UncompressedSize64 == 0 || entry.UncompressedSize64 > (256<<20)-total {
			return fmt.Errorf("unexpected release entry %q", name)
		}
		found[name] = true
		total += entry.UncompressedSize64
		target := filepath.Join(dir, name)
		if name == gameImage {
			target = image
		} else if target == image {
			return fmt.Errorf("release entry conflicts with running AppImage: %s", name)
		}
		st, e := os.Lstat(target)
		if name == cache && e == nil {
			continue
		}
		// Refuse symlinked parents before installing through them.
		rel := filepath.Dir(name)
		var parents []string
		for rel != "." {
			parents = append(parents, filepath.Join(dir, rel))
			rel = filepath.Dir(rel)
		}
		for j := len(parents) - 1; j >= 0; j-- {
			p := parents[j]
			if e := os.Mkdir(p, 0755); e != nil && !os.IsExist(e) {
				return e
			}
			st, e := os.Lstat(p)
			if e != nil {
				return e
			}
			if !st.IsDir() {
				return fmt.Errorf("not a directory: %s", p)
			}
		}
		source := filepath.Join(stage, strconv.Itoa(i))
		f := replacement{source: source, target: target}
		if e == nil {
			if !st.Mode().IsRegular() {
				return fmt.Errorf("not a regular file: %s", target)
			}
			f.previous = source + ".old"
			if e := backup(target, f.previous, st.Mode()); e != nil {
				return e
			}
		} else if !os.IsNotExist(e) {
			return e
		}
		input, e := entry.Open()
		if e != nil {
			return e
		}
		mode := os.FileMode(0644)
		if program {
			mode = 0755
		}
		e = writeFile(source, input, mode)
		input.Close()
		if e != nil {
			return e
		}
		files = append(files, f)
	}
	if !found[gameImage] || !found[debugImage] {
		return errors.New("release is missing an AppImage")
	}
	sort.SliceStable(files, func(i, j int) bool { return files[i].target != image && files[j].target == image })
	for i := range files {
		f := &files[i]
		if f.target == image && f.previous != "" {
			if err := os.Rename(f.previous, image+".old"); err != nil {
				return err
			}
			f.previous = image + ".old"
		}
		if err := os.Rename(f.source, f.target); err != nil {
			return err
		}
		committed++
	}
	return restart()
}

func apply(a asset, tag, image string) error {
	url := "https://github.com/" + repo + "/releases/download/" + tag + "/" + assetName
	data, err := fetch(url, a.Size, 3*time.Minute)
	if err != nil {
		return err
	}
	if int64(len(data)) != a.Size || fmt.Sprintf("sha256:%x", sha256.Sum256(data)) != a.Digest {
		return errors.New("release ZIP checksum mismatch")
	}
	return install(data, image, func() error {
		cmd := exec.Command(image, os.Args[1:]...)
		cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
		if err := cmd.Start(); err != nil {
			return err
		}
		cmd.Process.Release()
		return nil
	})
}

// Return true if the window was closed or the new game has already started.
func offer(entry, image, skip, tag string, a asset) bool {
	cmd := exec.Command(entry, "--update-offer", tag)
	cmd.Stderr = os.Stderr
	status, err := cmd.StdinPipe()
	if err != nil {
		return false
	}
	reply, err := cmd.StdoutPipe()
	if err != nil {
		return false
	}
	if cmd.Start() != nil {
		return false
	}
	var answer [1]byte
	if _, err = io.ReadFull(reply, answer[:]); err != nil {
		status.Close()
		return cmd.Wait() == nil
	}
	defer cmd.Wait()
	defer status.Close()
	switch answer[0] {
	case 'N':
		if err = os.MkdirAll(filepath.Dir(skip), 0755); err == nil {
			err = os.WriteFile(skip, []byte(tag), 0644)
		}
	case 'Y':
		if err = apply(a, tag, image); err == nil {
			return true
		}
	default:
		return false
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "Update: %v\n", err)
		fmt.Fprintf(status, "Update: %.900s", err.Error())
	}
	return false
}

func check(entry string) bool {
	path := os.Getenv("APPIMAGE")
	if !filepath.IsAbs(path) {
		return false
	}
	image, err := filepath.EvalSymlinks(path)
	if err != nil {
		return false
	}
	dir := filepath.Dir(image)
	game := dir
	switch {
	case len(os.Args) == 1:
	case len(os.Args) == 3 && os.Args[1] == "--game-dir":
		game = os.Args[2]
	default:
		return false
	}
	lock, err := os.Open(dir)
	if err != nil {
		return false
	}
	defer lock.Close()
	if syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB) != nil {
		return false
	}
	metadata, err := fetch("https://api.github.com/repos/"+repo+"/releases/latest", 1<<20, 3*time.Second)
	if err != nil {
		return false
	}
	var latest release
	if json.Unmarshal(metadata, &latest) != nil || !newer(latest.Tag) {
		return false
	}
	skip := filepath.Join(game, "_rehooked_", "settings", "skipped-release.txt")
	if ignored, _ := os.ReadFile(skip); string(ignored) == latest.Tag {
		return false
	}
	for _, a := range latest.Assets {
		if a.Name == assetName && a.Size > 0 && a.Size <= 128<<20 && digest.MatchString(a.Digest) {
			return offer(entry, image, skip, latest.Tag, a)
		}
	}
	return false
}

func main() {
	self, err := os.Executable()
	if err == nil {
		entry := filepath.Join(filepath.Dir(self), "AppRun.game")
		if check(entry) {
			return
		}
		err = syscall.Exec(entry, os.Args, os.Environ())
	}
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}
