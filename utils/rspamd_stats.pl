#!/usr/bin/env perl

use 5.010;
use Data::Dumper;
use Getopt::Long;
use Pod::Usage;
use warnings;
use strict;

my @symbols_search;
my @symbols_exclude;
my @symbols_bidirectional;
my $reject_score = 15.0;
my $junk_score = 6.0;
my $diff_alpha = 0.1;
my $correlations = 0;
my $log_file = "";
my $search_pattern = "";
my $startTime="";
my $endTime;
my $num_logs;
my $exclude_logs = 0;
my $man = 0;
my $help = 0;

# Associate file extensions with decompressors
my %decompressor = (
    'bz2' => 'bzip2 -cd',
    'gz'  => 'gzip -cd',
    'xz'  => 'xz -cd',
    'zst' => 'zstd -cd',
);

GetOptions(
  "reject-score|r=f" => \$reject_score,
  "junk-score|j=f" => \$junk_score,
  "symbol|s=s@" => \@symbols_search,
  "symbol-bidir|S=s@" => \@symbols_bidirectional,
  "exclude|X=s@" => \@symbols_exclude,
  "log|l=s" => \$log_file,
  "alpha-score|alpha|a=f" => \$diff_alpha,
  "correlations|c" => \$correlations,
  "search-pattern=s" => \$search_pattern,
  "start=s" => \$startTime,
  "end=s" => \$endTime,
  "num-logs|n=i" => \$num_logs,
  "exclude-logs|x=i" => \$exclude_logs,
  "help|?" => \$help,
  "man" => \$man
) or pod2usage(2);

pod2usage(1) if $help;
pod2usage(-exitval => 0, -verbose => 2) if $man;


# Global vars
my $total = 0;
my $total_spam = 0;
my $total_junk = 0;
my $junk_symbols = 0;
my $spam_symbols = 0;
my $ham_symbols = 0;
my $ham_spam_change = 0;
my $ham_junk_change = 0;
my %sym_res;
my $rspamd_log;
my $enabled = 0;

my %action;
my %timeStamp;
my %scanTime = (
    max   => 0,
    total => 0,
);
my %bidir_match;

# Convert bidirectional symbols
foreach my $s (@symbols_bidirectional) {
  $bidir_match{$s} = {
    spam => "${s}_SPAM",
    ham  => "${s}_HAM",
  };
  push @symbols_search, $s unless grep /^$s$/, @symbols_search;
}

@symbols_search = '.*'
  unless @symbols_search;

if ($log_file eq '-' || $log_file eq '') {
  $rspamd_log = \*STDIN;
  &ProcessLog();
}
elsif ( -d "$log_file" ) {
  my $log_dir = "$log_file";

  my @logs = &GetLogfilesList($log_dir);

  # Process logs
  foreach (@logs) {
    my $ext = (/[^.]+\.?([^.]*?)$/)[0];
    my $dc = $decompressor{$ext} || 'cat';

    open( $rspamd_log, "-|", "$dc $log_dir/$_" )
      or die "cannot execute $dc $log_dir/$_ : $!";

    &ProcessLog;

    close($rspamd_log)
      or warn "cannot close $dc $log_dir/$_: $!";
  }
}
else {
  open($rspamd_log, '<', $log_file) or die "cannot open $log_file";
  &ProcessLog();
}

my $total_ham = $total - ($total_spam + $total_junk);

if ($total > 0) {
  while (my ($s, $r) = each(%sym_res)) {
    if ($r->{hits} > 0) {
      my $th = $r->{hits};
      my $sh = $r->{spam_hits};
      my $jh = $r->{junk_hits};
      my $hh = $r->{hits} - $sh - $jh;
      my $htp = $hh * 100.0 / $total_ham if $total_ham != 0;
      my $stp = $sh * 100.0 / $total_spam if $total_spam != 0;
      my $jtp = $jh * 100.0 / $total_junk if $total_junk != 0;

      printf "%s   avg. weight %.3f, hits %d(%.3f%%):
  Ham  %7.3f%%, %6d/%-6d (%7.3f%%)
  Spam %7.3f%%, %6d/%-6d (%7.3f%%)
  Junk %7.3f%%, %6d/%-6d (%7.3f%%)
",
        $s, $r->{weight} / $r->{hits}, $th, ( $th / $total * 100 ),
        ( $hh / $th * 100 ), $hh, $total_ham,  ( $htp or 0 ),
        ( $sh / $th * 100 ), $sh, $total_spam, ( $stp or 0 ),
        ( $jh / $th * 100 ), $jh, $total_junk, ( $jtp or 0 );

      my $schp = $r->{spam_change} / $total_spam * 100.0 if $total_spam;
      my $jchp = $r->{junk_change} / $total_junk * 100.0 if $total_junk;

      if ($r->{weight} != 0) {
        if ($r->{weight} > 0) {
          printf "
Spam changes (ham/junk -> spam): %6d/%-6d (%7.3f%%)
Spam  changes / total spam hits: %6d/%-6d (%7.3f%%)
Junk changes      (ham -> junk): %6d/%-6d (%7.3f%%)
Junk  changes / total junk hits: %6d/%-6d (%7.3f%%)
",
            $r->{spam_change}, $th, ( $r->{spam_change} / $th * 100 ),
            $r->{spam_change}, $total_spam, ( $schp or 0 ),
            $r->{junk_change}, $th, ( $r->{junk_change} / $th * 100 ),
            $r->{junk_change}, $total_junk, ( $jchp or 0 );
        }
        else {
          printf "
Spam changes (spam -> junk/ham): %6d/%-6d (%7.3f%%)
Spam changes / total spam hits : %6d/%-6d (%7.3f%%)
Junk changes (junk -> ham)     : %6d/%-6d (%7.3f%%)
Junk changes / total junk hits : %6d/%-6d (%7.3f%%)
",
            $r->{spam_change}, $th, ( $r->{spam_change} / $th * 100 ),
            $r->{spam_change}, $total_spam, ( $schp or 0 ),
            $r->{junk_change}, $th, ( $r->{junk_change} / $th * 100 ),
            $r->{junk_change}, $total_junk, ( $jchp or 0 );
        }
      }

      if ($correlations) {
        print "Correlations report:\n";

        while (my ($cs,$hits) = each %{$r->{corr}}) {
          my $corr_prob = $hits / $total;
          my $sym_prob = $r->{hits} / $total;
          printf "Probability of %s when %s fires: %.3f\n", $s, $cs, ($corr_prob / $sym_prob);
        }
      }

    }
    else {
      print "Symbol $s has not been met\n";
    }

    print '-' x 80 . "\n";
  }
}

print "
=== Summary ", '=' x 68, "
Messages scanned: $total";
printf " [ %s / %s ]
", $timeStamp{'start'}, $timeStamp{'end'}
  if defined $timeStamp{'start'};
say '';
printf "%11s: %6.2f%%, %d\n", $_, 100 * $action{$_} / $total, $action{$_}
  for sort keys %action;
say '';
printf "scan time min/avg/max = %.2f/%.2f/%.2f s
", $scanTime{'min'} / 1000,
  ($total) ? $scanTime{'total'} / $total / 1000 : undef,
  $scanTime{'max'} / 1000
  if exists $scanTime{'min'};
say '=' x 80;

exit;

sub ProcessLog {
  while(<$rspamd_log>) {
    if (!$enabled && ($search_pattern eq "" || /$search_pattern/)) {
      $enabled = 1;
    }

    next if !$enabled;

    if (/^.*rspamd_task_write_log.*$/) {
      my $ts = join ' ', ( split /\s+/ )[ 0 .. 2 ];

      next if ( $ts lt $startTime );
      next if ( defined $endTime && $ts gt $endTime );

      if ($_ !~ /\(([^()]+)\): \[(NaN|-?\d+(?:\.\d+)?)\/(-?\d+(?:\.\d+)?)\]\s+\[([^\]]+)\].+? time: (\d+\.\d+)ms real/) {
        #print "BAD: $_\n";
        next;
      }

      my @symbols = split /(?:\{[^}]*\})?(?:$|,)/, $4;
      my $scan_time = $5;
      my $act = $1;
      my $score = $2 * 1.0;
      my $skip = 0;

      foreach my $ex (@symbols_exclude) {
        my @found = grep {/^$ex/} @symbols;

        if (scalar(@found) > 0) {
          $skip = 1;
          last;
        }
      }

      next if ( $skip != 0 );

      $timeStamp{'end'} = $ts;
      $timeStamp{'start'} //= $timeStamp{'end'};
      $scanTime{'min'} = $scan_time
        if ( !exists $scanTime{'min'} || $scanTime{'min'} > $scan_time );
      $scanTime{'max'} = $scan_time
        if ( $scanTime{'max'} < $scan_time );
      $scanTime{'total'} += $scan_time;

      $action{$act}++;
      $total ++;

      if ($score >= $reject_score) {
        $total_spam ++;
      }
      elsif ($score >= $junk_score) {
        $total_junk ++;
      }

      my @sym_names;

      foreach my $s (@symbols_search) {
        my @selected = grep /$s/, @symbols;

        if (scalar(@selected) > 0) {

          foreach my $sym (@selected) {
            $sym =~ /^([^\(]+)(\(([^\)]+)\))?/;
            my $sym_name = $1;
            my $sym_score = 0;
            my $orig_name = $sym_name;

            if ($2) {
              $sym_score = $3 * 1.0;

              if (abs($sym_score) < $diff_alpha) {
                next;
              }

              my $bm = $bidir_match{$sym_name};
              if ($bm) {
                if ($sym_score >= 0) {
                  $sym_name = $bm->{'spam'};
                }
                else {
                  $sym_name = $bm->{'ham'};
                }
              }
            }

            next if $orig_name !~ /^$s/;

            push @sym_names, $sym_name;

            if (!$sym_res{$sym_name}) {
              $sym_res{$sym_name} = {
                hits => 0,
                spam_hits => 0,
                junk_hits => 0,
                spam_change => 0,
                junk_change => 0,
                weight => 0,
                corr => {},
              };
            }

            my $r = $sym_res{$sym_name};

            $r->{hits} ++;
            $r->{weight} += $sym_score;
            my $is_spam = 0;
            my $is_junk = 0;

            if ($score >= $reject_score) {
              $is_spam = 1;
              $r->{spam_hits} ++;
            }
            elsif ($score >= $junk_score) {
              $is_junk = 1;
              $r->{junk_hits} ++;
            }

            if ($sym_score != 0) {
              my $score_without = $score - $sym_score;

              if ($sym_score > 0) {
                if ($is_spam && $score_without < $reject_score) {
                  $r->{spam_change} ++;
                }
                if ($is_junk && $score_without < $junk_score) {
                  $r->{junk_change} ++;
                }
              }
              else {
                if (!$is_spam && $score_without >= $reject_score) {
                  $r->{spam_change} ++;
                }
                if (!$is_junk && $score_without >= $junk_score) {
                  $r->{junk_change} ++;
                }
              }
            }
          } # End foreach symbols selected
        }
      }

      if ($correlations) {
        foreach my $sym (@sym_names) {
          my $r = $sym_res{$sym};

          foreach my $corr_sym (@sym_names) {
            if ($corr_sym ne $sym) {
              if ($r->{'corr'}->{$corr_sym}) {
                $r->{'corr'}->{$corr_sym} ++;
              }
              else {
                $r->{'corr'}->{$corr_sym} = 1;
              }
            }
          }
        } # End of correlations check
      }
    }
  }
}

sub GetLogfilesList {
    my ($dir) = @_;
    opendir( DIR, $dir ) or die $!;

    my $pattern = join( '|', keys %decompressor );
    my $re = qr/\.[0-9]+(?:\.(?:$pattern))?/;

    # Add unnumbered logs first
    my @logs =
      grep { -f "$dir/$_" && !/$re/ } readdir(DIR);

    # Add numbered logs
    rewinddir(DIR);
    push( @logs,
        ( sort numeric ( grep { -f "$dir/$_" && /$re/ } readdir(DIR) ) ) );

    closedir(DIR);

    # Select required logs and revers their order
    @logs =
      reverse
      splice( @logs, $exclude_logs, $num_logs ||= @logs - $exclude_logs );

    # Loop through array printing out filenames
    print "\nParsing log files:\n";
    foreach my $file (@logs) {
        print "  $file\n";
    }
    print "\n";

    return @logs;
}

sub numeric {
    $a =~ /\.(\d+)\./;
    my $a_num = $1;
    $b =~ /\.(\d+)\./;
    my $b_num = $1;

    $a_num <=> $b_num;
}

__END__

=head1 NAME

rspamd_stats - analyze Rspamd rules by parsing log files

=head1 SYNOPSIS

rspamd_stats [options] [--symbol=SYM1 [--symbol=SYM2...]] [--log file]

 Options:
   --log=file             log file or directory to read (stdin by default)
   --reject-score=score   set reject threshold (15 by default)
   --junk-score=score     set junk score (6.0 by default)
   --symbol=sym           check specified symbol (perl regexps, '.*' by default)
   --alpha-score=score    set ignore score for symbols (0.1 by default)
   --correlations         enable correlations report
   --search-pattern       do not process input unless the desired pattern is found
   --start                starting time (oldest) for log parsing
   --end                  ending time (newest) for log parsing
   --num-logs=integer     number of recent logfiles to analyze (all files in the directory by default)
   --exclude-logs=integer number of latest logs to exclude (0 by default)
   --help                 brief help message
   --man                  full documentation

=head1 OPTIONS

=over 8

=item B<--log>

Specifies log file or directory to read data from.
If a directory is specified B<rspamd_stats> analyses files in the directory
including known compressed file types. Number of log files can be limited using
B<--num-logs> and B<--exclude-logs> options. This assumes that files in the log
directory have B<newsyslog(8)>- or B<logrotate(8)>-like name format with numeric
indexes. Files without indexes (generally it is merely one file) are considered
the most recent and files with lower indexes are considered newer.

=item B<--reject-score>

Specifies the reject (spam) threshold.

=item B<--junk-score>

Specifies the junk (add header or rewrite subject) threshold.

=item B<--alpha-score>

Specifies the minimum score for a symbol to be considered by this script.

=item B<--symbol>

Add symbol or pattern (pcre format) to analyze.

=item B<--num-logs>

If set, limits number of analyzed logfiles in the directory to the specified value.

=item B<--exclude-logs>

Number of latest logs to exclude (0 by default).

=item B<--correlations>

Additionally print correlation rate for each symbol displayed. This routine calculates merely paired correlations between symbols.

=item B<--search-pattern>

Do not process input unless finding the specified regular expression. Useful to skip logs to a certain position.

=item  B<--exclude>

Exclude log lines if certain symbols are fired (e.g. GTUBE). You may specify this option multiple time to skip multiple symbols.

=item B<--start>

Select log entries after this time. Format: C<YYYY-MM-DD HH:MM:SS> (can be
truncated to any desired accuracy). If used with B<--end> select entries between
B<--start> and B<--end>.

=item B<--end>

Select log entries before this time. Format: C<YYYY-MM-DD HH:MM:SS> (can be
truncated to any desired accuracy). If used with B<--start> select entries between
B<--start> and B<--end>.

=item B<--help>

Print a brief help message and exits.

=item B<--man>

Prints the manual page and exits.

=back

=head1 DESCRIPTION

B<rspamd_stats> will read the given log file (or standard input) and provide statistics for the specified symbols:

    Symbol: BAYES_SPAM (weight 3.763) (381985 hits, 26.827%)
    Ham hits: 184557 (48.315%), total ham: 1095487 (ham with BAYES_SPAM: 16.847%)
    Spam hits: 15134 (3.962%), total spam: 16688 (spam with BAYES_SPAM: 90.688%)
    Junk hits: 182294 (47.723%), total junk: 311699 (junk with BAYES_SPAM: 58.484%)
    Spam changes (ham/junk -> spam): 7026 (1.839%), total percentage (changes / spam hits): 42.102%
    Junk changes (ham -> junk): 95192 (24.920%), total percentage (changes / junk hits): 30.540%

Where there are the following attributes:

=over 4

=item *

B<Weight>: average score for a symbols

=item *

B<Total hits>: total number of hits and percentage of symbol hits divided by total number of messages

=item *

B<HAM hits>: provides the following information about B<HAM> messages with the specified symbol (from left to right):

=over 4

=item 1.

B<total symbol hits>: number of messages that has this symbol and are B<HAM>

=item 2.

B<ham percentage>: number of symbol hits divided by overall B<HAM> messages count

=item 3.

B<total ham hits>: overall number of B<HAM> messages

=item 4.

B<ham with symbol percentage>: percentage of number of hits with specified symbol in B<HAM> messages divided by total number of B<HAM> messages.

=back

=item *

B<SPAM hits>: provides the following information about B<SPAM> messages - same as previous but for B<SPAM> class.

=item *

B<Junk hits>: provides the following information about B<Junk> messages - same as previous but for B<JUNK> class.

=item *

B<Spam changes>: displays data about how much messages switched their class because of the specific symbol weight.

=item *

B<Junk changes>: displays data about how much messages switched their class because of the specific symbol weight.

=back

=cut
