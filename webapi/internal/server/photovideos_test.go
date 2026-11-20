// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"database/sql"
	"encoding/json"
	"fmt"
	"testing"
)

// The videos of animated photos in GET /api/users/{id}/photos and GET
// /api/groups/{id}/photos (video_url, video_file_id), on the logger
// fixtures "sample" and "photos" with the videos set by each test.

const (
	videoStored   = 850101 // an animated photo's video, its bytes stored
	videoUnstored = 850102 // one whose bytes are not stored
)

// addPhotoVideos adds the two video files and sets the given history
// rows' videos, row id to file id, in the table.
func addPhotoVideos(t *testing.T, db *sql.DB, table string, videos map[uint64]uint64) {
	t.Helper()
	if _, err := db.Exec("INSERT INTO telegram_files (id, tg_file_id, tg_unique_id, file_type, " +
		"file_size, on_disk, sha256, file_ext, orig_file_name, hit_count, created_at, updated_at, " +
		"accessed_at) VALUES " +
		"(850101, 'CgACAgUAAxkBAAIvideo01', 'AQADvideo01', 'animation', 409600, 1, " +
		"UNHEX(SHA2('photo videos 850101', 256)), 'mp4', '', 1, '2026-07-01 10:00:00', " +
		"'2026-07-01 10:00:00', '2026-11-17 09:00:00'), " +
		"(850102, 'CgACAgUAAxkBAAIvideo02', 'AQADvideo02', 'animation', 409600, 0, " +
		"NULL, 'mp4', '', 0, '2026-07-01 10:00:00', '2026-07-01 10:00:00', " +
		"'2026-07-01 10:00:00')"); err != nil {
		t.Fatal(err)
	}
	for row, file := range videos {
		if _, err := db.Exec("UPDATE "+table+" SET video_file_id = ? WHERE id = ?", file, row); err != nil {
			t.Fatal(err)
		}
	}
}

type tPhotoVideo struct {
	ID          uint64
	FileID      *uint64 `json:"file_id"`
	VideoURL    *string `json:"video_url"`
	VideoFileID *uint64 `json:"video_file_id"`
}

// checkPhotoVideos checks every item's video against want, item id to
// "<video file id>:<has a URL>" ("none" for no video), and that the
// keys are there even when null.
func checkPhotoVideos(t *testing.T, e *chatEnv, path string, want map[uint64]string) {
	t.Helper()
	_, _, body := e.get(path, e.as("admin"))
	var raw struct{ Items []map[string]json.RawMessage }
	var p struct{ Items []tPhotoVideo }
	if err := json.Unmarshal(body, &raw); err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(body, &p); err != nil {
		t.Fatal(err)
	}
	if len(p.Items) != len(want) {
		t.Fatalf("%s: items %s", path, body)
	}
	for i, it := range p.Items {
		for _, k := range []string{"video_url", "video_file_id"} {
			if _, ok := raw.Items[i][k]; !ok {
				t.Errorf("%s: item %d without %s", path, it.ID, k)
			}
		}
		got := "none"
		if it.VideoFileID != nil {
			got = fmt.Sprintf("%d:%v", *it.VideoFileID, it.VideoURL != nil)
		} else if it.VideoURL != nil {
			got = "url without a file id"
		}
		w, ok := want[it.ID]
		if !ok || got != w {
			t.Errorf("%s: item %d video %s, want %s", path, it.ID, got, w)
		}
		if it.VideoURL != nil && *it.VideoURL != "/files/"+e.tokens.Encrypt(*it.VideoFileID) {
			t.Errorf("%s: item %d video_url %s", path, it.ID, *it.VideoURL)
		}
		// The still stays the url, never the video.
		if it.FileID == nil || (it.VideoFileID != nil && *it.FileID == *it.VideoFileID) {
			t.Errorf("%s: item %d file_id %v", path, it.ID, it.FileID)
		}
	}
}

func TestUserPhotoVideos(t *testing.T) {
	t.Parallel()
	e, _ := newPhotosEnv(t)
	// The current photo's video is on an older row of its file (1001;
	// the newest, 1004, has none), 1005's is not stored and cannot be
	// served (the tests have no daemon), 1003 is a still.
	addPhotoVideos(t, e.ro, "telegram_user_hist_profile_photo",
		map[uint64]uint64{1001: videoStored, 1005: videoUnstored})
	checkPhotoVideos(t, e, photosPath(fotoBanyak), map[uint64]string{
		1004: "850101:true",
		1005: "850102:false",
		1003: "none",
	})
	// Another user's video does not leak into this one's photos of the
	// same file.
	checkPhotoVideos(t, e, photosPath(fotoLain), map[uint64]string{1006: "none"})
	// The same access as before.
	e.decode(photosPath(fotoBanyak), e.as("researcher"), 404, nil)
}

func TestGroupPhotoVideos(t *testing.T) {
	t.Parallel()
	e, _ := newPhotosEnv(t)
	path := func(id int64) string { return fmt.Sprintf("/api/groups/%d/photos", id) }
	addPhotoVideos(t, e.ro, "telegram_group_hist_photo",
		map[uint64]uint64{1003: videoStored, 1001: videoUnstored})
	checkPhotoVideos(t, e, path(gnuWeeb), map[uint64]string{
		1003: "850101:true",
		1001: "850102:false",
		1004: "none",
	})
	e.decode(path(gnuWeeb), e.as("researcher"), 404, nil)
}

// A video recorded without its bytes, as the daemon records them, has
// its link all the same when the daemon may fetch it, and the link has
// the daemon fetch the video's file.
func TestPhotoVideoFetched(t *testing.T) {
	t.Parallel()
	e := newPhotoListEnv(t)
	if _, err := e.ro.Exec("INSERT INTO telegram_files (id, tg_file_id, tg_unique_id, " +
		"file_type, file_size, on_disk, sha256, file_ext, orig_file_name, hit_count, " +
		"created_at, updated_at, accessed_at) VALUES (851101, 'CgACAgUAAxkBAAIvideo11', " +
		"'AQADvideo11', 'video', 409600, 0, NULL, 'mp4', '', 0, '2026-09-01 16:39:11', " +
		"'2026-09-01 16:39:11', '2026-09-01 16:39:11')"); err != nil {
		t.Fatal(err)
	}
	if _, err := e.ro.Exec("UPDATE telegram_user_hist_profile_photo SET video_file_id = 851101 " +
		"WHERE id = 1102"); err != nil {
		t.Fatal(err)
	}
	checkPhotoVideos(t, e.chatEnv, photosPath(fotoBaru), map[uint64]string{
		0:    "none",
		1102: "851101:true",
		1101: "none",
	})
	code, _, body := e.get("/files/"+e.tokens.Encrypt(851101), e.as("admin"))
	if code != 200 || string(body) != "photo 851001" {
		t.Errorf("video: %d %q", code, body)
	}
	if got := e.fake.requests("POST /v1/files/851101/fetch"); len(got) != 1 {
		t.Errorf("video fetches %q", got)
	}
}
