# SPDX-License-Identifier: MIT
#
# A CineForm recording to its two deliverables: AV1 and FLAC in a .webm, and the CineForm
# frames unchanged with FLAC in a .mkv, each with a .cff beside it, checked before either is
# called done.
#
#   elixir deliver.exs <in.cfhd> <out-dir> --meta <meta.exs> [--av1mkv <exe>] [--gpu <name>]
#   elixir deliver.exs --self-test
#
# <meta.exs> evaluates to a keyword list of title, abstract, authors, date, keywords and
# references, in the shapes a CITATION.cff uses.

defmodule Deliver do
  @moduledoc false

  @required [:title, :abstract, :authors, :date]

  def frames(path) do
    case File.open(path, [:read, :binary]) do
      {:ok, io} ->
        try do
          case chunks(io, 0, File.stat!(path).size, 0) do
            {:ok, 0} -> {:error, "#{Path.basename(path)} holds no 00dc video chunk"}
            other -> other
          end
        after
          File.close(io)
        end

      {:error, why} ->
        {:error, "cannot read #{path}: #{why}"}
    end
  end

  defp chunks(_io, pos, stop, n) when pos >= stop, do: {:ok, n}

  defp chunks(_io, pos, stop, _n) when stop - pos < 8,
    do: {:error, "a chunk header at byte #{pos} is cut short"}

  defp chunks(io, pos, stop, n) do
    {:ok, <<id::binary-size(4), len::little-32>>} = :file.pread(io, pos, 8)
    finish = pos + 8 + len
    next = min(finish + rem(len, 2), stop)

    cond do
      finish > stop ->
        {:error, "#{id} at byte #{pos} runs #{finish - stop} bytes past its container"}

      id in ["RIFF", "LIST"] ->
        with {:ok, m} <- chunks(io, pos + 12, finish, 0), do: chunks(io, next, stop, n + m)

      true ->
        chunks(io, next, stop, n + if(id == "00dc", do: 1, else: 0))
    end
  end

  def video_blocks(info) do
    video = for [_, t] <- Regex.scan(~r/^track (\d+): type 1,/m, info), do: t

    blocks =
      Map.new(Regex.scan(~r/^track (\d+): (\d+) blocks,/m, info), fn [_, t, b] ->
        {t, String.to_integer(b)}
      end)

    case video do
      [t] when is_map_key(blocks, t) -> {:ok, blocks[t]}
      _ -> {:error, "expected one video track with a block count, read #{length(video)}"}
    end
  end

  def missing(meta), do: Enum.reject(@required, &(meta[&1] not in [nil, "", []]))

  def name(author) do
    a = Map.new(author, fn {k, v} -> {to_string(k), v} end)
    a["name"] || a["alias"] || String.trim("#{a["given-names"]} #{a["family-names"]}")
  end

  defp xml(s),
    do:
      s
      |> to_string()
      |> String.replace("&", "&amp;")
      |> String.replace("<", "&lt;")
      |> String.replace(">", "&gt;")
      |> String.replace("\"", "&quot;")

  def xmp(meta) do
    creators = Enum.map_join(meta[:authors], fn a -> "<rdf:li>#{xml(name(a))}</rdf:li>" end)

    """
    <?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>
    <x:xmpmeta xmlns:x="adobe:ns:meta/">
     <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
      <rdf:Description rdf:about="" xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:xmp="http://ns.adobe.com/xap/1.0/">
       <dc:title><rdf:Alt><rdf:li xml:lang="x-default">#{xml(meta[:title])}</rdf:li></rdf:Alt></dc:title>
       <dc:description><rdf:Alt><rdf:li xml:lang="x-default">#{xml(meta[:abstract])}</rdf:li></rdf:Alt></dc:description>
       <dc:creator><rdf:Seq>#{creators}</rdf:Seq></dc:creator>
       <xmp:CreateDate>#{xml(meta[:date])}</xmp:CreateDate>
      </rdf:Description>
     </rdf:RDF>
    </x:xmpmeta>
    <?xpacket end="w"?>
    """
  end

  def scalar(v) when is_number(v), do: to_string(v)

  def scalar(v) do
    escaped =
      v
      |> to_string()
      |> String.replace("\\", "\\\\")
      |> String.replace("\"", "\\\"")
      |> String.replace("\n", "\\n")

    "\"#{escaped}\""
  end

  def yaml(pairs, pad \\ ""),
    do: Enum.map_join(pairs, fn {k, v} -> field(to_string(k), v, pad) end)

  defp field(k, v, pad) when is_map(v), do: "#{pad}#{k}:\n" <> yaml(v, pad <> "  ")

  defp field(k, v, pad) when is_list(v) do
    if v != [] and Keyword.keyword?(v),
      do: "#{pad}#{k}:\n" <> yaml(v, pad <> "  "),
      else: "#{pad}#{k}:\n" <> Enum.map_join(v, &item(&1, pad <> "  "))
  end

  defp field(k, v, pad), do: "#{pad}#{k}: #{scalar(v)}\n"

  defp item(v, pad) when is_map(v) or (is_list(v) and v != []) do
    [first | rest] = String.split(yaml(v, pad <> "  "), "\n", trim: true)
    "#{pad}- #{String.trim_leading(first)}\n" <> Enum.map_join(rest, &(&1 <> "\n"))
  end

  defp item(v, pad), do: "#{pad}- #{scalar(v)}\n"

  def citation(meta, file, sha256, what) do
    yaml(
      "cff-version": "1.2.0",
      message: "If you use this recording, please cite it as below.",
      type: "dataset",
      title: "#{meta[:title]} (#{Path.extname(file)})",
      abstract: "#{meta[:abstract]} #{what}",
      authors: meta[:authors],
      "date-released": meta[:date],
      keywords: meta[:keywords] || [],
      identifiers: [
        [
          type: "other",
          value: "sha256:#{sha256}",
          description: "SHA-256 of #{Path.basename(file)}"
        ]
      ],
      references: meta[:references] || []
    )
  end

  def run(exe, args) do
    {out, code} = System.cmd(exe, args, stderr_to_stdout: true)
    {code, out}
  rescue
    e -> {127, Exception.message(e)}
  end

  def sha256(path),
    do:
      File.stream!(path, 1_048_576)
      |> Enum.reduce(:crypto.hash_init(:sha256), &:crypto.hash_update(&2, &1))
      |> :crypto.hash_final()
      |> Base.encode16(case: :lower)
end

defmodule Deliver.Run do
  @moduledoc false
  import Deliver

  def main(cfhd, dir, meta_path, exe, gpu) do
    {meta, _} = Code.eval_file(meta_path)

    pre = [
      {File.regular?(cfhd), "the recording #{cfhd} exists"},
      {File.regular?(exe), "av1mkv is built at #{exe}"},
      {missing(meta) == [], "the meta names #{Enum.join(missing(meta), ", ")}"}
    ]

    for {false, what} <- pre, do: IO.puts("  FAIL precondition: #{what}")
    if Enum.any?(pre, &(not elem(&1, 0))), do: System.halt(1)

    n =
      case report(frames(cfhd), "the .cfhd's frames, counted from its RIFF chunks") do
        {:ok, n} -> n
        _ -> System.halt(1)
      end

    File.mkdir_p!(dir)
    stem = Path.basename(cfhd, Path.extname(cfhd))
    webm = Path.join(dir, stem <> ".webm")
    mkv = Path.join(dir, stem <> ".mkv")

    tmp =
      Path.join(
        System.tmp_dir!(),
        "deliver-#{:os.getpid()}-#{System.unique_integer([:positive])}"
      )

    File.mkdir_p!(tmp)
    xmp = Path.join(tmp, "meta.xmp")
    File.write!(xmp, xmp(meta))

    try do
      cut = Path.join(tmp, "cut.cfhd")
      {:ok, _} = :file.copy(cfhd, cut, div(File.stat!(cfhd).size, 2))

      steps = [
        {"av1mkv mkv", fn -> ok(run(exe, ["mkv", cfhd, mkv, "--xmp", xmp])) end},
        {"av1mkv check: the .mkv carries the .cfhd's frames unchanged",
         fn -> ok(run(exe, ["check", cfhd, mkv])) end},
        {"control: a .cfhd cut to half is refused by av1mkv check",
         fn -> refused(run(exe, ["check", cut, mkv])) end},
        {"control: a .cfhd cut to half is refused by the RIFF count",
         fn -> refused(frames(cut)) end},
        {"av1mkv encode",
         fn -> ok(run(exe, ["encode", cfhd, webm, "--xmp", xmp, "--gpu", gpu])) end},
        {".mkv video blocks equal the .cfhd's #{n} frames, XMP whole",
         fn -> same(exe, mkv, n) end},
        {".webm video blocks equal the .cfhd's #{n} frames, XMP whole",
         fn -> same(exe, webm, n) end}
      ]

      failed = Enum.count(steps, fn {what, step} -> elem(report(step.(), what), 0) != :ok end)

      if failed > 0 do
        IO.puts("\n#{failed} of #{length(steps)} step(s) failed; no .cff written.")
        System.halt(1)
      end

      for {file, what} <- [
            {mkv,
             "CineForm frames as recorded, byte for byte, with the audio as FLAC, in a Matroska file: #{n} frames."},
            {webm, "AV1 from NVENC with the audio as FLAC, in a WebM: #{n} frames."}
          ] do
        sum = sha256(file)
        File.write!(file <> ".cff", citation(meta, file, sum, what))

        IO.puts(
          "  wrote #{Path.basename(file)} (#{div(File.stat!(file).size, 1_000_000)} MB, sha256 #{sum}) and its .cff"
        )
      end
    after
      File.rm_rf(tmp)
    end
  end

  defp ok({0, _}), do: {:ok, nil}
  defp ok({code, out}), do: {:error, "exit #{code}: #{String.slice(String.trim(out), -300, 300)}"}
  defp refused({0, _}), do: {:error, "accepted"}
  defp refused({:ok, n}), do: {:error, "accepted, counting #{n}"}
  defp refused(_), do: {:ok, nil}

  defp same(exe, file, n) do
    with {0, info} <- run(exe, ["info", file]),
         {:ok, ^n} <- video_blocks(info),
         true <- String.contains?(info, "a whole xpacket") || {:error, "no whole XMP tag"} do
      {:ok, n}
    else
      {:ok, m} ->
        {:error, "#{m} video blocks"}

      {code, out} when is_integer(code) ->
        {:error, "info exit #{code}: #{String.slice(out, 0, 200)}"}

      other ->
        other
    end
  end

  defp report({:ok, v} = r, what) do
    IO.puts("  ok   #{what}#{if is_integer(v), do: ": #{v}", else: ""}")
    r
  end

  defp report({:error, why} = r, what) do
    IO.puts("  FAIL #{what}: #{why}")
    r
  end
end

defmodule Deliver.SelfTest do
  @moduledoc false
  import Deliver

  defp chunk(id, data),
    do:
      <<id::binary, byte_size(data)::little-32, data::binary>> <>
        if(rem(byte_size(data), 2) == 1, do: <<0>>, else: "")

  defp list(form, inner), do: chunk("LIST", form <> inner)

  defp avi(movi),
    do:
      chunk(
        "RIFF",
        "AVI " <>
          list("hdrl", chunk("avih", <<0::448>>)) <> list("movi", movi) <> chunk("idx1", "")
      )

  defp counted(bytes) do
    path = Path.join(System.tmp_dir!(), "deliver-test-#{System.unique_integer([:positive])}.cfhd")
    File.write!(path, bytes)

    try do
      frames(path)
    after
      File.rm(path)
    end
  end

  @info """
  track 1: type 1, codec V_AV1, 1152x648, frame rate 0.000
  track 2: type 2, codec A_FLAC, 48000 Hz, 2 ch, 16 bit
  track 1: 289 blocks, 10 key, 900 bytes, first 0.000 s, last 9.600 s
  track 2: 120 blocks, 120 key, 800 bytes, first 0.000 s, last 9.600 s
  """

  def run do
    IO.puts("\ndeliver.exs self-test")

    good =
      avi(
        chunk("00dc", "abc") <> chunk("01wb", "xy") <> chunk("00dc", "d") <> chunk("00dc", "ef")
      )

    meta = [
      title: "T",
      abstract: "A",
      authors: [["family-names": "Lee", "given-names": "K. S. Ernest (iFire)"]],
      date: "2026-10-01"
    ]

    controls = [
      {"positive: three 00dc chunks with odd sizes are counted", counted(good) == {:ok, 3}},
      {"positive: one video track's blocks are read from av1mkv info",
       video_blocks(@info) == {:ok, 289}},
      {"positive: a complete meta names nothing missing", missing(meta) == []},
      {"negative: a file cut short is refused",
       match?({:error, _}, counted(binary_part(good, 0, byte_size(good) - 3)))},
      {"negative: a file with no 00dc chunk is refused",
       match?({:error, _}, counted(avi(chunk("01wb", "xy"))))},
      {"negative: a chunk longer than its container is refused",
       match?({:error, _}, counted(chunk("RIFF", "AVI " <> <<"00dc", 99::little-32>>)))},
      {"negative: info with no video track is refused",
       match?({:error, _}, video_blocks(String.replace(@info, "type 1,", "type 2,")))},
      {"negative: info with two video tracks is refused",
       match?({:error, _}, video_blocks(String.replace(@info, "type 2,", "type 1,")))},
      {"negative: a meta without a title is refused",
       missing(Keyword.delete(meta, :title)) == [:title]},
      {"negative: a quote and a newline cannot break out of a YAML scalar",
       scalar("a\"b\nc") == ~S("a\"b\nc")},
      {"negative: markup in a name cannot break out of the XMP",
       not String.contains?(xmp(Keyword.put(meta, :authors, [[name: "<x>"]])), "<x>")}
    ]

    for {what, ok} <- controls, do: IO.puts("  #{if ok, do: "ok  ", else: "FAIL"} #{what}")
    bad = Enum.count(controls, &(not elem(&1, 1)))

    IO.puts(
      if bad == 0,
        do: "  #{length(controls)} of #{length(controls)} held.",
        else: "  #{bad} control(s) failed."
    )

    if bad == 0, do: 0, else: 1
  end
end

case System.argv() do
  ["--self-test" | _] ->
    System.halt(Deliver.SelfTest.run())

  [cfhd, dir | opts] ->
    {o, _, _} = OptionParser.parse(opts, strict: [meta: :string, av1mkv: :string, gpu: :string])
    here = Path.dirname(__ENV__.file)
    exe = o[:av1mkv] || Path.join([here, "build", "av1mkv.exe"])

    unless o[:meta] do
      IO.puts("  FAIL precondition: --meta <meta.exs> names the recording")
      System.halt(1)
    end

    Deliver.Run.main(cfhd, dir, o[:meta], exe, o[:gpu] || "RTX 4090")

  _ ->
    IO.puts(
      "usage: elixir deliver.exs <in.cfhd> <out-dir> --meta <meta.exs> [--av1mkv <exe>] [--gpu <name>]"
    )

    System.halt(1)
end
